/**
 * @file    joystick_example.c
 * @brief   Ejemplo de uso del driver YXA197 con FreeRTOS en STM32F103RB
 *
 * Este archivo muestra cómo integrar la librería joystick_yxa197 en un proyecto
 * generado con STM32CubeMX. NO es un archivo ejecutable por sí solo; los
 * fragmentos marcados con "SECCIÓN N" deben copiarse en los archivos
 * correspondientes del proyecto CubeIDE (generalmente main.c).
 *
 * Configuración de hardware usada en este ejemplo:
 *   VRX  → PA0  (ADC1_IN0, Arduino A0, CN7-28)
 *   VRY  → PA1  (ADC1_IN1, Arduino A1, CN7-30)
 *   SW   → PB5  (GPIO Input Pull-Up, CN10-29)
 *   BTN1 → PB4  (GPIO Input Pull-Up, CN10-27)  [opcional]
 *   LED  → PA5  (LED verde integrado del Nucleo F103RB)
 *
 * Configuración de CubeMX requerida: ver README.md (sección "Configuración
 * en STM32CubeMX") para los pasos exactos.
 *
 * =========================================================================
 * SECCIÓN 1 — Includes y declaraciones globales
 * (pegar al inicio de main.c, en la zona "USER CODE BEGIN Includes")
 * =========================================================================
 */

#include "joystick_yxa197.h"
#include "usart.h"     /* para imprimir por UART (opcional) */
#include <stdio.h>
#include <string.h>

/*
 * Cola de eventos del joystick — accesible desde la tarea de usuario.
 * Declarar como variable global para que sea visible en main() y en la tarea.
 */
static QueueHandle_t xJoyQueue;

/* =========================================================================
 * SECCIÓN 2 — Inicialización del driver
 * (crear esta función y llamarla desde main(), después de todas las llamadas
 * MX_*_Init() y antes de vTaskStartScheduler())
 * =========================================================================
 */

static void Joystick_AppInit(void)
{
    /* --- 2.1  Crear la cola de eventos ----------------------------------- */
    /*
     * La cola debe crearse ANTES de Joystick_Init().
     * Tamaño: 8 eventos (ajustar si la tarea consumidora puede ser lenta).
     * Elemento: JoystickEvent_t (ver joystick_yxa197.h).
     */
    xJoyQueue = xQueueCreate(8U, sizeof(JoystickEvent_t));
    if (xJoyQueue == NULL) {
        /* Error de heap: aumentar configTOTAL_HEAP_SIZE en FreeRTOSConfig.h */
        while (1) {
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            HAL_Delay(100);
        }
    }

    /* --- 2.2  Configurar el driver --------------------------------------- */
    JoystickConfig_t cfg;
    Joystick_GetDefaultConfig(&cfg);  /* Rellena threshold=1500, prio=2, etc. */

    /* ADC: hadc1 generado por CubeMX para ADC1 */
    cfg.hadc      = &hadc1;
    cfg.x_channel = ADC_CHANNEL_0;   /* VRX conectado a PA0 */
    cfg.y_channel = ADC_CHANNEL_1;   /* VRY conectado a PA1 */

    /*
     * Inversión de ejes (opcional):
     * Si al mover el joystick a la derecha se reporta IZQUIERDA → invert_x = 1.
     * Si al mover hacia arriba se reporta ABAJO              → invert_y = 1.
     * Probar con el ejemplo de UART (Sección 4) para verificar la orientación.
     */
    cfg.invert_x = 0;
    cfg.invert_y = 0;

    /*
     * Umbral de dirección: 1500 de 4095.
     * Reducir (ej. 800) para mayor sensibilidad; aumentar (ej. 2000) para
     * más zona muerta. Calibrar midiendo el valor crudo en reposo y en extremos.
     */
    cfg.threshold = 1500U;

    /* Botón SW del joystick en PB5 (activo en bajo = GPIO_PIN_RESET con Pull-Up) */
    cfg.buttons[JOY_BTN_SW].port         = GPIOB;
    cfg.buttons[JOY_BTN_SW].pin          = GPIO_PIN_5;
    cfg.buttons[JOY_BTN_SW].active_state = GPIO_PIN_RESET;

    /*
     * Botón externo adicional en PB4 (opcional).
     * Si no se usa, mantener button_count = 1.
     */
    cfg.buttons[JOY_BTN_1].port         = GPIOB;
    cfg.buttons[JOY_BTN_1].pin          = GPIO_PIN_4;
    cfg.buttons[JOY_BTN_1].active_state = GPIO_PIN_RESET;

    cfg.button_count   = 2U;          /* SW + BTN1 */
    cfg.event_queue    = xJoyQueue;
    cfg.task_priority  = 2U;
    cfg.poll_period_ms = 20U;         /* 50 Hz — debounce efectivo: 60 ms */

    /* --- 2.3  Inicializar el driver -------------------------------------- */
    if (Joystick_Init(&cfg) != pdPASS) {
        /* Error al crear la tarea interna: revisar configTOTAL_HEAP_SIZE */
        while (1) {
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            HAL_Delay(200);
        }
    }
}

/* =========================================================================
 * SECCIÓN 3 — Tarea de aplicación: reacción a eventos del joystick
 * (declarar su prototipo y crear la tarea en main() antes del scheduler)
 *
 * Esta tarea muestra el patrón recomendado: esperar eventos de la cola
 * de forma bloqueante (portMAX_DELAY) y reaccionar a cada uno.
 * =========================================================================
 */

#define APP_TASK_STACK_WORDS  256U
#define APP_TASK_PRIORITY     3U    /* Mayor prioridad que el driver (prio 2) */

void vJoystickAppTask(void *pvParameters)
{
    (void)pvParameters;

    JoystickEvent_t ev;
    char uart_buf[64];
    int  len;

    for (;;)
    {
        /*
         * Esperar el próximo evento de forma bloqueante.
         * La tarea cede el CPU hasta que el driver publique algo en la cola.
         */
        if (xQueueReceive(xJoyQueue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* ---- Reaccionar según el tipo de evento ------------------------- */
        switch (ev.type)
        {
            case JOY_EVENT_DIRECTION_CHANGE:
                /*
                 * El joystick cambió de posición.
                 * ev.direction: nueva dirección lógica.
                 * ev.x_raw / ev.y_raw: lectura ADC cruda en el momento.
                 */
                len = snprintf(uart_buf, sizeof(uart_buf),
                               "[JOY] Dirección: %s  (X=%u Y=%u)\r\n",
                               Joystick_DirectionToString(ev.direction),
                               ev.x_raw, ev.y_raw);
                HAL_UART_Transmit(&huart2, (uint8_t *)uart_buf, (uint16_t)len,
                                  HAL_MAX_DELAY);

                /* Encender LED cuando el joystick está arriba */
                if (ev.direction == JOY_DIR_UP ||
                    ev.direction == JOY_DIR_UP_LEFT ||
                    ev.direction == JOY_DIR_UP_RIGHT)
                {
                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
                }
                else
                {
                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
                }
                break;

            case JOY_EVENT_BUTTON_PRESS:
                len = snprintf(uart_buf, sizeof(uart_buf),
                               "[JOY] Botón %d PRESIONADO\r\n",
                               (int)ev.button);
                HAL_UART_Transmit(&huart2, (uint8_t *)uart_buf, (uint16_t)len,
                                  HAL_MAX_DELAY);

                /* El botón SW (índice 0) enciende el LED al presionarse */
                if (ev.button == JOY_BTN_SW) {
                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
                }
                break;

            case JOY_EVENT_BUTTON_RELEASE:
                len = snprintf(uart_buf, sizeof(uart_buf),
                               "[JOY] Botón %d SOLTADO\r\n",
                               (int)ev.button);
                HAL_UART_Transmit(&huart2, (uint8_t *)uart_buf, (uint16_t)len,
                                  HAL_MAX_DELAY);

                if (ev.button == JOY_BTN_SW) {
                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
                }
                break;

            default:
                break;
        }
    }
}

/* =========================================================================
 * SECCIÓN 4 — Tarea de calibración / diagnóstico (opcional)
 *
 * Útil durante el desarrollo: imprime los valores ADC crudos cada 500 ms
 * para verificar el rango de movimiento y calibrar el threshold.
 * Deshabilitar o eliminar en la versión final del proyecto.
 * =========================================================================
 */

#define CALIBRATION_PERIOD_MS  500U

void vJoystickCalibTask(void *pvParameters)
{
    (void)pvParameters;

    uint16_t x, y;
    char     uart_buf[80];
    int      len;
    TickType_t xLastWake = xTaskGetTickCount();

    for (;;)
    {
        Joystick_GetRaw(&x, &y);

        len = snprintf(uart_buf, sizeof(uart_buf),
                       "[CALIB] X=%4u  Y=%4u  Dir=%-12s  SW=%d\r\n",
                       x, y,
                       Joystick_DirectionToString(Joystick_GetDirection()),
                       Joystick_IsButtonPressed(JOY_BTN_SW));
        HAL_UART_Transmit(&huart2, (uint8_t *)uart_buf, (uint16_t)len,
                          HAL_MAX_DELAY);

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(CALIBRATION_PERIOD_MS));
    }
}

/* =========================================================================
 * SECCIÓN 5 — Fragmento para main() (referencia)
 *
 * Pegar dentro de int main(void), en la zona "USER CODE BEGIN 2",
 * después de todas las llamadas MX_*_Init():
 * =========================================================================
 *
 *   // Inicializa el driver y crea la cola de eventos
 *   Joystick_AppInit();
 *
 *   // Tarea principal que reacciona a eventos (prio 3 > prio del driver 2)
 *   xTaskCreate(vJoystickAppTask,
 *               "JoyApp",
 *               APP_TASK_STACK_WORDS,
 *               NULL,
 *               APP_TASK_PRIORITY,
 *               NULL);
 *
 *   // Tarea de calibración (opcional, solo en desarrollo)
 *   xTaskCreate(vJoystickCalibTask,
 *               "JoyCalib",
 *               256,
 *               NULL,
 *               1,
 *               NULL);
 *
 *   vTaskStartScheduler();
 *
 * =========================================================================
 * SECCIÓN 6 — Uso alternativo: polling sin cola
 *
 * Si no se necesitan eventos y solo se quiere leer el estado actual,
 * se puede usar la API getter directamente desde cualquier tarea:
 * =========================================================================
 *
 * void vMiTarea(void *pvParameters)
 * {
 *     TickType_t xLast = xTaskGetTickCount();
 *     for (;;)
 *     {
 *         JoystickDirection_t dir = Joystick_GetDirection();
 *         uint8_t sw_pressed      = Joystick_IsButtonPressed(JOY_BTN_SW);
 *         uint16_t x, y;
 *         Joystick_GetRaw(&x, &y);
 *
 *         // usar dir, sw_pressed, x, y en la lógica de la aplicación...
 *
 *         vTaskDelayUntil(&xLast, pdMS_TO_TICKS(50));
 *     }
 * }
 *
 * =========================================================================
 * SECCIÓN 7 — Notas de integración
 *
 *  · Los archivos joystick_yxa197.h y joystick_yxa197.c deben copiarse a
 *    Core/Inc/ y Core/Src/ respectivamente en el proyecto CubeIDE.
 *
 *  · Si CubeMX regenera el código, los archivos del driver no se tocan.
 *    Solo verificar que el código en zonas "USER CODE" de main.c persista.
 *
 *  · La variable huart2 y hadc1 son generadas por CubeMX. Verificar que
 *    USART2 y ADC1 estén habilitados en el .ioc del proyecto.
 *
 *  · Para mayor heap, modificar configTOTAL_HEAP_SIZE en FreeRTOSConfig.h
 *    (mínimo recomendado con este driver: 6144 bytes).
 */
