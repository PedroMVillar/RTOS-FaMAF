/**
 * @file    hcsr04_example.c
 * @brief   Ejemplo de uso del driver HC-SR04 con FreeRTOS en STM32F103RB
 *
 * Este archivo muestra cómo integrar la librería hcsr04 en un proyecto
 * generado con STM32CubeMX. NO es un archivo ejecutable por sí solo;
 * los fragmentos deben copiarse en los archivos correspondientes del
 * proyecto CubeIDE (main.c, tim.c, etc.).
 *
 * Configuración de pines usada en este ejemplo:
 *   - TRIG : PA8  (GPIO Output)
 *   - ECHO : PA0  (TIM2_CH1, Input Capture)
 *   - LED  : PA5  (GPIO Output, LED verde del Nucleo F103RB)
 *
 * =========================================================================
 * SECCIÓN 1 — Includes y declaraciones globales
 * (poner al inicio de main.c junto a los demás includes generados por Cube)
 * =========================================================================
 */

/* --- Incluye la librería ------------------------------------------------ */
#include "hcsr04.h"
#include "usart.h"   /* si se quiere imprimir por UART */
#include <stdio.h>
#include <string.h>

/* --- Handle global del sensor ------------------------------------------ */
/* Se declara global para que sea accesible tanto desde la tarea como desde
   el callback HAL_TIM_IC_CaptureCallback. */
HCSR04_Handle_t hsonar;

/* =========================================================================
 * SECCIÓN 2 — Inicialización del sensor
 * (llamar en main(), después de MX_TIM2_Init() y antes de osKernelStart())
 * =========================================================================
 */
static void Sonar_Init(void)
{
    HCSR04_Status_t status;

    /*
     * TRIG: PA8 configurado en CubeMX como GPIO Output.
     *       El handle del GPIO generado es: TRIG_GPIO_Port / TRIG_Pin
     *       (depende del "User Label" que hayas puesto en CubeMX).
     *       Ajusta según tu proyecto.
     *
     * ECHO: PA0 = TIM2_CH1. El handle htim2 y TIM_CHANNEL_1 son los
     *       generados por CubeMX automáticamente.
     */
    status = HCSR04_Init(&hsonar,
                          GPIOA, GPIO_PIN_8,   /* TRIG: PA8            */
                          &htim2, TIM_CHANNEL_1 /* ECHO: TIM2_CH1/PA0  */
                          );

    if (status != HCSR04_OK) {
        /* Error crítico: bucle de fallo visible por el LED rojo del Nucleo */
        while (1) {
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            HAL_Delay(100);
        }
    }
}

/* =========================================================================
 * SECCIÓN 3 — Callback de Input Capture HAL
 * (pegar en main.c o en el archivo de callbacks del proyecto)
 *
 * CubeMX genera el cuerpo vacío de esta función en main.c con la etiqueta
 * "USER CODE BEGIN 4". Agrega allí la llamada a HCSR04_CaptureCallback.
 * =========================================================================
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    /*
     * Despacha el evento al driver. Si tienes otros periféricos usando
     * Input Capture, agrega más llamadas similares aquí; la función
     * filtra por instancia de timer internamente.
     */
    HCSR04_CaptureCallback(&hsonar, htim);
}

/* =========================================================================
 * SECCIÓN 4 — Tarea FreeRTOS de medición de distancia
 * (crear la tarea en main() antes de osKernelStart() y declarar su prototipo)
 * =========================================================================
 */

/*
 * Tarea: mide distancia cada 200 ms y:
 *   - Enciende el LED si el objeto está a < 20 cm.
 *   - Envía el resultado por UART2 (opcional, comentar si no se usa).
 *
 * Stack: 256 palabras (1 KB). Ajustar si se agregan más operaciones.
 * Prioridad: 2 (normal). Subir si se necesita más determinismo.
 */
#define SONAR_TASK_STACK_WORDS  256U
#define SONAR_TASK_PRIORITY     2U
#define SONAR_PERIOD_MS         200U
#define SONAR_ALERT_CM          20.0f

void vSonarTask(void *pvParameters)
{
    (void)pvParameters;   /* no se usa en este ejemplo */

    float         distance_cm = 0.0f;
    HCSR04_Status_t status;
    char          uart_buf[64];
    TickType_t    xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        /* Mide la distancia. La tarea se bloquea aquí hasta que la ISR
           complete la captura o expire el timeout (50 ms por defecto). */
        status = HCSR04_Measure(&hsonar, &distance_cm);

        if (status == HCSR04_OK) {
            /* --- Lógica de aplicación --------------------------------- */
            if (distance_cm < SONAR_ALERT_CM) {
                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);   /* LED ON  */
            } else {
                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); /* LED OFF */
            }

            /* --- Envío opcional por UART (115200 baud) ---------------- */
            int len = snprintf(uart_buf, sizeof(uart_buf),
                               "Distancia: %.1f cm\r\n", distance_cm);
            HAL_UART_Transmit(&huart2, (uint8_t *)uart_buf, (uint16_t)len,
                              HAL_MAX_DELAY);
        }
        else if (status == HCSR04_TIMEOUT) {
            /* Sin objeto en rango o sensor desconectado */
            int len = snprintf(uart_buf, sizeof(uart_buf),
                               "Sin objeto detectado\r\n");
            HAL_UART_Transmit(&huart2, (uint8_t *)uart_buf, (uint16_t)len,
                              HAL_MAX_DELAY);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
        }

        /*
         * vTaskDelayUntil garantiza un período constante de SONAR_PERIOD_MS
         * sin acumular deriva. Es preferible a vTaskDelay(N) porque este
         * último mide desde el final de la iteración, no desde el inicio.
         */
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SONAR_PERIOD_MS));
    }
}

/* =========================================================================
 * SECCIÓN 5 — Fragmento para main() (referencia)
 *
 * Pegar dentro de int main(void), en la zona "USER CODE BEGIN 2" de CubeMX,
 * después de todas las llamadas MX_*_Init():
 * =========================================================================
 *
 *   Sonar_Init();
 *
 *   xTaskCreate(vSonarTask,
 *               "Sonar",
 *               SONAR_TASK_STACK_WORDS,
 *               NULL,
 *               SONAR_TASK_PRIORITY,
 *               NULL);
 *
 *   vTaskStartScheduler();   // o osKernelStart() si usás CMSIS-RTOS
 *
 * =========================================================================
 * SECCIÓN 6 — Ejemplo con dos sensores simultáneos
 * =========================================================================
 *
 * Si necesitás dos HC-SR04 (ej. frente y atrás), usá dos handles y dos
 * timers o dos canales del mismo timer:
 *
 *   HCSR04_Handle_t hsonar_front, hsonar_rear;
 *
 *   HCSR04_Init(&hsonar_front, GPIOA, GPIO_PIN_8, &htim2, TIM_CHANNEL_1);
 *   HCSR04_Init(&hsonar_rear,  GPIOB, GPIO_PIN_6, &htim3, TIM_CHANNEL_1);
 *
 *   // En HAL_TIM_IC_CaptureCallback:
 *   void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
 *       HCSR04_CaptureCallback(&hsonar_front, htim);
 *       HCSR04_CaptureCallback(&hsonar_rear,  htim);
 *   }
 *
 *   // IMPORTANTE: no medir ambos sensores simultáneamente.
 *   // El ultrasonido de un sensor puede interferir con el otro.
 *   // Medirlos de forma alternada con un retardo de al menos 60 ms.
 */
