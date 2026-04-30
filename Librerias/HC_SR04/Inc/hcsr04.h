/**
 * @file    hcsr04.h
 * @brief   Driver FreeRTOS para sensor ultrasónico HC-SR04 en STM32
 *
 * Arquitectura del driver:
 *   - TRIG: salida GPIO. Se genera un pulso de 10 µs usando el contador DWT.
 *   - ECHO: entrada de Input Capture de un timer HAL. Una ISR captura el
 *     flanco de subida (inicio del pulso), cambia la polaridad y captura el
 *     flanco de bajada (fin del pulso). Al terminar, libera un semáforo
 *     binario de FreeRTOS para despertar la tarea que llamó a
 *     HCSR04_Measure().
 *
 * La ISR es deliberadamente corta (solo lectura de registro + give del
 * semáforo). Todo el cálculo se hace en contexto de tarea.
 *
 * Configuración requerida en STM32CubeMX (ver README.md para detalles):
 *   - TRIG: GPIO Output, Push-Pull, sin pull-up/down, velocidad Low.
 *   - ECHO: canal de Input Capture de un TIM (ej. TIM2_CH1).
 *       · Prescaler : 71   (72 MHz / 72 = 1 MHz → 1 tick = 1 µs)
 *       · ARR       : 65535
 *       · Polarity  : Rising Edge
 *       · NVIC: interrupción global del timer habilitada.
 *   - FreeRTOS habilitado con vía "CMSIS-RTOS v1" o directamente.
 *
 * Uso básico:
 * @code
 *   HCSR04_Handle_t hsonar;
 *   HCSR04_Init(&hsonar, TRIG_GPIO_Port, TRIG_Pin, &htim2, TIM_CHANNEL_1);
 *
 *   // Dentro de HAL_TIM_IC_CaptureCallback:
 *   HCSR04_CaptureCallback(&hsonar, htim);
 *
 *   // En la tarea FreeRTOS:
 *   float distancia_cm;
 *   if (HCSR04_Measure(&hsonar, &distancia_cm) == HCSR04_OK) { ... }
 * @endcode
 */

#ifndef HCSR04_H
#define HCSR04_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"      /* GPIO_TypeDef, HAL, SystemCoreClock, etc. */
#include "tim.h"       /* TIM_HandleTypeDef                         */
#include "FreeRTOS.h"
#include "semphr.h"

/* ------------------------------------------------------------------ */
/*  Parámetros ajustables                                              */
/* ------------------------------------------------------------------ */

/** Timeout de medición en ms. El HC-SR04 devuelve ~38 ms si no hay
 *  objeto. Se agrega margen para cubrir latencias del scheduler. */
#define HCSR04_TIMEOUT_MS       50U

/** Velocidad del sonido a 20 °C en cm/µs. */
#define HCSR04_SOUND_SPEED      0.0343f

/** Distancia máxima reportable en cm (rango real del sensor: ~400 cm). */
#define HCSR04_MAX_DISTANCE_CM  400.0f

/* ------------------------------------------------------------------ */
/*  Tipos                                                              */
/* ------------------------------------------------------------------ */

/** Códigos de retorno de la API. */
typedef enum {
    HCSR04_OK      = 0, /**< Medición exitosa.                        */
    HCSR04_TIMEOUT = 1, /**< No se recibió respuesta del sensor.      */
    HCSR04_ERROR   = 2  /**< Error de inicialización u otro fallo.    */
} HCSR04_Status_t;

/**
 * @brief Handle del sensor. Debe vivir mientras se use el sensor.
 *
 * Los campos prefijados con '_' son internos; no modificar directamente.
 */
typedef struct {
    /* --- Configuración de usuario (escritura solo en Init) --------- */
    GPIO_TypeDef    *trig_port;   /**< Puerto GPIO del pin TRIG.      */
    uint16_t         trig_pin;    /**< Máscara del pin TRIG.          */
    TIM_HandleTypeDef *htim;      /**< Timer con Input Capture.       */
    uint32_t         channel;     /**< Canal del timer (TIM_CHANNEL_x)*/

    /* --- Estado interno (solo escrito por ISR y HCSR04_Measure) --- */
    volatile uint32_t _t_rise;    /**< Tick capturado en flanco subida*/
    volatile uint32_t _t_fall;    /**< Tick capturado en flanco bajada*/
    volatile uint8_t  _edge_idx;  /**< 0=esperando subida, 1=bajada  */
    SemaphoreHandle_t _sem;       /**< Semáforo binario FreeRTOS.     */
} HCSR04_Handle_t;

/* ------------------------------------------------------------------ */
/*  API pública                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Inicializa el handle y los recursos FreeRTOS del sensor.
 *
 * Habilita el contador DWT (necesario para el delay de 10 µs en TRIG),
 * crea el semáforo binario interno y fuerza el pin TRIG en bajo.
 * Debe llamarse antes del inicio del scheduler o desde una tarea.
 *
 * @param  hdev       Puntero al handle del sensor.
 * @param  trig_port  Puerto GPIO del pin TRIG.
 * @param  trig_pin   Pin TRIG (ej. GPIO_PIN_9).
 * @param  htim       Timer HAL configurado en modo Input Capture.
 * @param  channel    Canal del timer (ej. TIM_CHANNEL_1).
 * @retval HCSR04_OK si la inicialización fue exitosa, HCSR04_ERROR si
 *         no se pudo crear el semáforo.
 */
HCSR04_Status_t HCSR04_Init(HCSR04_Handle_t   *hdev,
                              GPIO_TypeDef      *trig_port,
                              uint16_t           trig_pin,
                              TIM_HandleTypeDef *htim,
                              uint32_t           channel);

/**
 * @brief  Realiza una medición de distancia (bloqueante para la tarea).
 *
 * Genera el pulso TRIG, inicia la captura por interrupción y suspende
 * la tarea llamante hasta que la ISR complete la medición o expire el
 * timeout. Las demás tareas FreeRTOS continúan ejecutándose.
 *
 * @note   Solo llamar desde una tarea FreeRTOS, nunca desde una ISR.
 *
 * @param  hdev         Puntero al handle del sensor (ya inicializado).
 * @param  distance_cm  Puntero donde se almacena el resultado en cm.
 * @retval HCSR04_OK      si la medición fue válida.
 *         HCSR04_TIMEOUT si el sensor no respondió en HCSR04_TIMEOUT_MS.
 *         HCSR04_ERROR   si hdev o distance_cm son NULL.
 */
HCSR04_Status_t HCSR04_Measure(HCSR04_Handle_t *hdev, float *distance_cm);

/**
 * @brief  Callback que debe llamarse desde HAL_TIM_IC_CaptureCallback.
 *
 * Verifica que el htim del callback corresponda al timer del handle.
 * Si es así, captura el valor del registro CCR según el flanco actual,
 * cambia la polaridad para el siguiente flanco y, al completar la
 * medición, libera el semáforo desde la ISR.
 *
 * Ejemplo de uso:
 * @code
 *   void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
 *       HCSR04_CaptureCallback(&hsonar, htim);
 *   }
 * @endcode
 *
 * @param  hdev  Puntero al handle del sensor.
 * @param  htim  Parámetro recibido por HAL_TIM_IC_CaptureCallback.
 */
void HCSR04_CaptureCallback(HCSR04_Handle_t *hdev, TIM_HandleTypeDef *htim);

#ifdef __cplusplus
}
#endif

#endif /* HCSR04_H */
