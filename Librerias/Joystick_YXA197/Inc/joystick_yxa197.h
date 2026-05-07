/**
 * @file    joystick_yxa197.h
 * @brief   Driver FreeRTOS para módulo joystick analógico YXA197 en STM32
 *
 * Arquitectura del driver:
 *   - Los ejes X/Y son potenciómetros analógicos leídos con el ADC del MCU.
 *     La tarea interna reconfigura el canal ADC antes de cada lectura usando
 *     HAL_ADC_ConfigChannel (conversión individual sin DMA ni modo Scan).
 *     CubeMX debe configurar ADC1 en modo Single Conversion (sin DMA).
 *   - El botón del joystick (SW) y hasta 3 botones adicionales se leen por
 *     polling de GPIO con anti-rebote por contador de histéresis.
 *   - Una tarea FreeRTOS interna hace polling cada poll_period_ms ms usando
 *     vTaskDelayUntil y publica JoystickEvent_t en una cola del usuario al
 *     detectar cambios de dirección o de estado de botón.
 *   - No se usan ISRs, osDelay ni la capa CMSIS-RTOS: solo FreeRTOS nativo.
 *
 * Señales del módulo YXA197 y conexiones sugeridas para Nucleo STM32F103RB:
 *   VCC  → 3.3 V  (pin CN6 o CN7)
 *   GND  → GND    (pin CN6 o CN7)
 *   VRX  → PA0    (ADC1_IN0, CN7-28, Arduino A0)
 *   VRY  → PA1    (ADC1_IN1, CN7-30, Arduino A1)
 *   SW   → PB5    (GPIO Input con Pull-Up, CN10-29)
 *
 * Botones adicionales opcionales (si el módulo los incluye):
 *   BTN1 → PB4    (GPIO Input con Pull-Up, CN10-27)
 *   BTN2 → PB3    (GPIO Input con Pull-Up, CN10-25)
 *
 * Ver README.md para la configuración detallada de STM32CubeMX.
 *
 * Uso básico:
 * @code
 *   // Crear la cola antes del scheduler (o antes de Joystick_Init)
 *   QueueHandle_t xJoyQueue = xQueueCreate(8, sizeof(JoystickEvent_t));
 *
 *   // Configurar el driver
 *   JoystickConfig_t cfg;
 *   Joystick_GetDefaultConfig(&cfg);
 *   cfg.hadc       = &hadc1;
 *   cfg.x_channel  = ADC_CHANNEL_0;           // VRX → PA0
 *   cfg.y_channel  = ADC_CHANNEL_1;           // VRY → PA1
 *   cfg.buttons[JOY_BTN_SW].port         = GPIOB;
 *   cfg.buttons[JOY_BTN_SW].pin          = GPIO_PIN_5;
 *   cfg.buttons[JOY_BTN_SW].active_state = GPIO_PIN_RESET; // activo en bajo
 *   cfg.button_count = 1;
 *   cfg.event_queue  = xJoyQueue;
 *   Joystick_Init(&cfg);
 *
 *   // En la tarea del usuario:
 *   JoystickEvent_t ev;
 *   if (xQueueReceive(xJoyQueue, &ev, portMAX_DELAY) == pdTRUE) {
 *       if (ev.type == JOY_EVENT_DIRECTION_CHANGE) {
 *           // ev.direction contiene la nueva dirección
 *       } else if (ev.type == JOY_EVENT_BUTTON_PRESS) {
 *           // ev.button indica qué botón se presionó
 *       }
 *   }
 * @endcode
 */

#ifndef JOYSTICK_YXA197_H
#define JOYSTICK_YXA197_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"      /* GPIO_TypeDef, HAL, etc. — generado por CubeMX  */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* ------------------------------------------------------------------ */
/*  Parámetros ajustables en tiempo de compilación                     */
/* ------------------------------------------------------------------ */

/** Cantidad máxima de botones soportados (SW del joystick + opcionales). */
#define JOYSTICK_MAX_BUTTONS       4U

/** Palabras de stack de la tarea interna (32 bits c/u → 256 × 4 = 1 KB). */
#define JOYSTICK_TASK_STACK_WORDS  256U

/** Período de polling por defecto en milisegundos (50 Hz de muestreo). */
#define JOYSTICK_POLL_PERIOD_MS    20U

/** Timeout máximo de espera por una conversión ADC en milisegundos. */
#define JOYSTICK_ADC_TIMEOUT_MS    10U

/**
 * Tiempo de muestreo ADC por canal.
 * 28.5 ciclos ADC a 12 MHz ≈ 2.4 µs de muestreo + 1 µs de conversión = ~3.4 µs total.
 * Usar 239.5 ciclos para señales de alta impedancia (> 10 kΩ fuente).
 */
#define JOYSTICK_ADC_SAMPLETIME    ADC_SAMPLETIME_28CYCLES_5

/**
 * Valor central esperado en el ADC de 12 bits (4096 / 2 = 2048).
 * El joystick en reposo debería leer aproximadamente este valor.
 * Calibrar si el centro real difiere (medir con UART en reposo).
 */
#define JOYSTICK_CENTER_VALUE      2048U

/**
 * Desviación mínima del centro (en cuentas ADC) para registrar una dirección.
 * Aumentar si se detectan falsas direcciones en reposo; disminuir para
 * mayor sensibilidad. Rango recomendado: 800–2000 sobre 4095 total.
 */
#define JOYSTICK_THRESHOLD_DEFAULT 1500U

/**
 * Cantidad de lecturas consecutivas requeridas para confirmar el estado
 * de un botón (anti-rebote por histéresis).
 * Tiempo efectivo = DEBOUNCE_COUNT × POLL_PERIOD_MS.
 * Con los valores por defecto: 3 × 20 ms = 60 ms de debounce.
 */
#define JOYSTICK_DEBOUNCE_COUNT    3U

/* ------------------------------------------------------------------ */
/*  Tipos                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Dirección lógica del joystick derivada de los ejes analógicos.
 *
 * Las diagonales se generan cuando ambos ejes superan el umbral simultáneamente.
 */
typedef enum {
    JOY_DIR_CENTER     = 0,
    JOY_DIR_UP         = 1,
    JOY_DIR_DOWN       = 2,
    JOY_DIR_LEFT       = 3,
    JOY_DIR_RIGHT      = 4,
    JOY_DIR_UP_LEFT    = 5,
    JOY_DIR_UP_RIGHT   = 6,
    JOY_DIR_DOWN_LEFT  = 7,
    JOY_DIR_DOWN_RIGHT = 8,
} JoystickDirection_t;

/** Tipo de evento generado por el driver y publicado en la cola. */
typedef enum {
    JOY_EVENT_DIRECTION_CHANGE = 0, /**< El joystick cambió de posición.       */
    JOY_EVENT_BUTTON_PRESS     = 1, /**< Un botón fue presionado (debounced).  */
    JOY_EVENT_BUTTON_RELEASE   = 2, /**< Un botón fue soltado (debounced).     */
} JoystickEventType_t;

/** Identificador de botón. El índice 0 siempre es el SW del módulo joystick. */
typedef enum {
    JOY_BTN_SW = 0, /**< Botón SW integrado del joystick (presión vertical). */
    JOY_BTN_1  = 1, /**< Botón externo opcional 1.                          */
    JOY_BTN_2  = 2, /**< Botón externo opcional 2.                          */
    JOY_BTN_3  = 3, /**< Botón externo opcional 3.                          */
} JoystickButtonId_t;

/**
 * @brief Evento publicado en la cola del usuario por la tarea interna.
 *
 * Leer el campo @p direction cuando @p type == JOY_EVENT_DIRECTION_CHANGE.
 * Leer el campo @p button   cuando @p type == BUTTON_PRESS o BUTTON_RELEASE.
 * Los campos @p x_raw e @p y_raw siempre contienen la lectura del momento.
 */
typedef struct {
    JoystickEventType_t type;
    union {
        JoystickDirection_t direction; /**< Nueva dirección (DIRECTION_CHANGE).*/
        JoystickButtonId_t  button;    /**< Botón involucrado (PRESS/RELEASE). */
    };
    uint16_t x_raw; /**< Lectura cruda del eje X en el momento del evento (0–4095). */
    uint16_t y_raw; /**< Lectura cruda del eje Y en el momento del evento (0–4095). */
} JoystickEvent_t;

/**
 * @brief Configuración GPIO de un botón individual.
 *
 * @p active_state define el nivel lógico que corresponde a "botón presionado":
 *   - GPIO_PIN_RESET → activo en bajo (pull-up interno/externo, lo más común).
 *   - GPIO_PIN_SET   → activo en alto (pull-down o lógica positiva).
 */
typedef struct {
    GPIO_TypeDef  *port;
    uint16_t       pin;
    GPIO_PinState  active_state;
} JoystickButtonCfg_t;

/**
 * @brief Configuración completa del driver.
 *
 * Rellenar todos los campos requeridos y luego llamar a Joystick_Init().
 * Usar Joystick_GetDefaultConfig() para pre-inicializar con valores seguros.
 */
typedef struct {
    /* ---- ADC ----------------------------------------------------------- */
    ADC_HandleTypeDef *hadc;       /**< Handle del ADC (ej. &hadc1). [REQUERIDO] */
    uint32_t           x_channel;  /**< Canal ADC del eje X (ej. ADC_CHANNEL_0). [REQUERIDO] */
    uint32_t           y_channel;  /**< Canal ADC del eje Y (ej. ADC_CHANNEL_1). [REQUERIDO] */

    /**
     * Flags de inversión de eje. Usar si las direcciones físicas son opuestas
     * a las lógicas (depende de la orientación de montaje del módulo).
     *   invert_x = 1: empujar a la derecha reporta JOY_DIR_LEFT → invertir.
     *   invert_y = 1: empujar hacia arriba reporta JOY_DIR_DOWN → invertir.
     */
    uint8_t invert_x;
    uint8_t invert_y;

    /* ---- Botones ------------------------------------------------------- */
    /**
     * Configuración de cada botón. El índice 0 (JOY_BTN_SW) es el pulsador
     * integrado del módulo; los índices 1–3 son opcionales.
     * Rellenar los primeros @p button_count elementos. [REQUERIDO al menos 1]
     */
    JoystickButtonCfg_t buttons[JOYSTICK_MAX_BUTTONS];
    uint8_t             button_count; /**< Cuántos elementos de @p buttons usar (1–4). */

    /* ---- Umbral analógico ---------------------------------------------- */
    /**
     * Desviación mínima desde JOYSTICK_CENTER_VALUE (en cuentas ADC) para
     * considerar que el joystick está desplazado. Valores menores = CENTER.
     * Default: JOYSTICK_THRESHOLD_DEFAULT (1500 de 4095).
     */
    uint16_t threshold;

    /* ---- FreeRTOS ------------------------------------------------------- */
    /**
     * Cola FreeRTOS donde el driver publica JoystickEvent_t.
     * Crear con xQueueCreate(N, sizeof(JoystickEvent_t)) antes de Init.
     * Si la cola está llena, el evento se descarta sin bloquear la tarea.
     * [REQUERIDO]
     */
    QueueHandle_t event_queue;

    /** Prioridad de la tarea interna de polling. Default: 2. */
    UBaseType_t task_priority;

    /**
     * Período de polling en milisegundos. También determina la resolución
     * temporal del anti-rebote: tiempo_debounce = DEBOUNCE_COUNT × poll_period_ms.
     * Default: JOYSTICK_POLL_PERIOD_MS (20 ms → 50 Hz).
     */
    uint32_t poll_period_ms;
} JoystickConfig_t;

/* ------------------------------------------------------------------ */
/*  API pública                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Inicializa el driver y crea la tarea interna de polling FreeRTOS.
 *
 * Copia la configuración internamente, inicializa el estado y lanza la tarea.
 * Puede llamarse antes de vTaskStartScheduler() o desde una tarea de arranque.
 *
 * @param  config  Configuración del driver (copiada internamente; el puntero
 *                 no necesita mantenerse válido tras el retorno).
 * @return pdPASS si la tarea se creó correctamente; pdFAIL si algún argumento
 *         requerido es NULL o si el heap de FreeRTOS está agotado.
 *
 * @warning No llamar más de una vez (sin reinicialización del sistema).
 * @warning El hadc apuntado no debe ser usado por otros módulos mientras
 *          el driver esté activo, ya que la tarea reconfigura sus canales.
 */
BaseType_t Joystick_Init(const JoystickConfig_t *config);

/**
 * @brief  Rellena una JoystickConfig_t con valores por defecto seguros.
 *
 * Tras llamar a esta función, el usuario debe asignar obligatoriamente:
 * hadc, x_channel, y_channel, buttons[], button_count y event_queue.
 *
 * @param  config  Puntero al struct a inicializar (no puede ser NULL).
 */
void Joystick_GetDefaultConfig(JoystickConfig_t *config);

/**
 * @brief  Retorna la última dirección calculada del joystick.
 *
 * Alternativa a la cola para consultar el estado actual sin esperar eventos.
 * Se actualiza cada período de polling desde la tarea interna.
 *
 * @return Dirección actual (JoystickDirection_t).
 */
JoystickDirection_t Joystick_GetDirection(void);

/**
 * @brief  Copia las últimas lecturas ADC crudas a los punteros indicados.
 *
 * Útil para calibración o para mostrar valores numéricos por UART.
 *
 * @param[out] x  Valor crudo del eje X (0–4095). Puede ser NULL.
 * @param[out] y  Valor crudo del eje Y (0–4095). Puede ser NULL.
 */
void Joystick_GetRaw(uint16_t *x, uint16_t *y);

/**
 * @brief  Retorna 1 si el botón indicado está presionado (post-debounce).
 *
 * Thread-safe para lectura: accede a uint8_t volatile (lectura atómica).
 *
 * @param  btn  Identificador del botón (JOY_BTN_SW, JOY_BTN_1, ...).
 * @return 1 si presionado, 0 si suelto o si @p btn está fuera de rango.
 */
uint8_t Joystick_IsButtonPressed(JoystickButtonId_t btn);

/**
 * @brief  Convierte una JoystickDirection_t a cadena en español (debug/UART).
 *
 * @param  dir  Dirección a convertir.
 * @return Puntero a string literal en flash (ej. "ARRIBA", "DERECHA").
 *         Nunca retorna NULL.
 */
const char *Joystick_DirectionToString(JoystickDirection_t dir);

#ifdef __cplusplus
}
#endif

#endif /* JOYSTICK_YXA197_H */
