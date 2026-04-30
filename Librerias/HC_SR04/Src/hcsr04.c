/**
 * @file    hcsr04.c
 * @brief   Implementación del driver HC-SR04 para FreeRTOS en STM32
 *
 * Principio de funcionamiento del HC-SR04:
 *   1. El MCU genera un pulso de 10 µs en TRIG.
 *   2. El sensor emite 8 pulsos ultrasónicos a 40 kHz.
 *   3. El sensor pone ECHO en alto al emitir y en bajo al recibir el eco.
 *   4. La distancia se calcula como:
 *          d [cm] = t_echo [µs] × 0.0343 / 2
 *      donde 0.0343 cm/µs es la velocidad del sonido a ~20 °C.
 *
 * Estrategia de medición (ISR + semáforo):
 *   - Input Capture del timer con cambio de polaridad en la ISR.
 *   - La tarea llama a HCSR04_Measure(), que:
 *       a) Arma la captura (flanco de subida).
 *       b) Genera el pulso TRIG de 10 µs con DWT.
 *       c) Se bloquea en xSemaphoreTake() (cede el CPU).
 *   - La ISR (HCSR04_CaptureCallback):
 *       a) 1er disparo: guarda t_rise, cambia a flanco de bajada.
 *       b) 2do disparo: guarda t_fall, para IC, llama xSemaphoreGiveFromISR.
 *   - La tarea despierta, calcula la diferencia de ticks y obtiene la
 *     distancia en cm.
 *
 * Manejo de overflow del timer (contador de 16 bits):
 *   Si t_fall < t_rise el contador dio vuelta durante el pulso. La
 *   diferencia se corrige con: (65535 - t_rise) + t_fall + 1.
 *   Con ARR=65535 y 1 MHz esto ocurre solo si el eco supera 65.5 ms,
 *   lo cual está fuera del rango del sensor (38 ms máximo).
 */

#include "hcsr04.h"
#include "cmsis_os.h"   /* portYIELD_FROM_ISR, etc. (FreeRTOS a través de CMSIS) */

/* ------------------------------------------------------------------ */
/*  Delay por DWT (Data Watchpoint and Trace)                         */
/*                                                                     */
/*  El timer DWT/CYCCNT cuenta ciclos de CPU. Se usa solo para el     */
/*  pulso TRIG de 10 µs (busy-wait aceptable a esa escala).           */
/*  HAL_Delay tiene resolución de 1 ms, insuficiente.                  */
/* ------------------------------------------------------------------ */

/**
 * @brief  Habilita el contador de ciclos del DWT (Cortex-M3/M4/M7).
 *
 * Debe llamarse una sola vez antes de usar HCSR04_DelayUs().
 * En Cortex-M3 (STM32F1), DWT requiere que TRCENA esté activo.
 */
static void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; /* habilita DWT    */
    DWT->CYCCNT       = 0U;                           /* resetea contador*/
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;      /* activa CYCCNT  */
}

/**
 * @brief  Busy-wait de microsegundos usando DWT.
 *
 * @warning Solo llamar para duraciones muy cortas (≤ 100 µs). Para
 *          retardos más largos en tareas, usar vTaskDelay().
 *
 * @param  us  Microsegundos a esperar.
 */
static void HCSR04_DelayUs(uint32_t us)
{
    uint32_t start  = DWT->CYCCNT;
    /* Convierte µs a ciclos de CPU según la frecuencia real del sistema */
    uint32_t cycles = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < cycles) {
        /* espera activa — intencional y acotada a 10 µs */
    }
}

/* ------------------------------------------------------------------ */
/*  Implementación de la API pública                                   */
/* ------------------------------------------------------------------ */

HCSR04_Status_t HCSR04_Init(HCSR04_Handle_t   *hdev,
                              GPIO_TypeDef      *trig_port,
                              uint16_t           trig_pin,
                              TIM_HandleTypeDef *htim,
                              uint32_t           channel)
{
    if (hdev == NULL || trig_port == NULL || htim == NULL) {
        return HCSR04_ERROR;
    }

    /* Guarda configuración de hardware */
    hdev->trig_port = trig_port;
    hdev->trig_pin  = trig_pin;
    hdev->htim      = htim;
    hdev->channel   = channel;

    /* Estado interno inicial */
    hdev->_t_rise   = 0U;
    hdev->_t_fall   = 0U;
    hdev->_edge_idx = 0U;

    /* Habilita DWT para los delays de 10 µs */
    DWT_Init();

    /* TRIG en bajo (estado de reposo del sensor) */
    HAL_GPIO_WritePin(hdev->trig_port, hdev->trig_pin, GPIO_PIN_RESET);

    /* Crea el semáforo binario usado para sincronizar ISR ↔ tarea */
    hdev->_sem = xSemaphoreCreateBinary();
    if (hdev->_sem == NULL) {
        /* FreeRTOS no pudo reservar memoria para el semáforo */
        return HCSR04_ERROR;
    }

    return HCSR04_OK;
}

HCSR04_Status_t HCSR04_Measure(HCSR04_Handle_t *hdev, float *distance_cm)
{
    if (hdev == NULL || distance_cm == NULL || hdev->_sem == NULL) {
        return HCSR04_ERROR;
    }

    /* Descarta cualquier give previo no consumido (medición anterior) */
    xSemaphoreTake(hdev->_sem, 0);

    /* Prepara la máquina de estados de la ISR para flanco de subida */
    hdev->_edge_idx = 0U;

    /* Configura el canal para capturar en flanco de subida y activa IC */
    __HAL_TIM_SET_CAPTUREPOLARITY(hdev->htim, hdev->channel,
                                   TIM_INPUTCHANNELPOLARITY_RISING);
    HAL_TIM_IC_Start_IT(hdev->htim, hdev->channel);

    /* Genera el pulso TRIG: mínimo 10 µs según datasheet */
    HAL_GPIO_WritePin(hdev->trig_port, hdev->trig_pin, GPIO_PIN_SET);
    HCSR04_DelayUs(12U);   /* 12 µs da margen sobre el mínimo de 10 µs  */
    HAL_GPIO_WritePin(hdev->trig_port, hdev->trig_pin, GPIO_PIN_RESET);

    /*
     * Bloquea la tarea esperando el semáforo que libera la ISR.
     * El scheduler cede la CPU a otras tareas durante este tiempo.
     * Tiempo máximo de espera: HCSR04_TIMEOUT_MS (cubre los 38 ms del
     * sensor más margen de scheduling).
     */
    if (xSemaphoreTake(hdev->_sem, pdMS_TO_TICKS(HCSR04_TIMEOUT_MS))
            == pdFALSE)
    {
        /* La ISR no completó la captura en tiempo: sensor ausente o
         * sin objeto en rango. Se detiene IC para no dejar el timer
         * en un estado inconsistente. */
        HAL_TIM_IC_Stop_IT(hdev->htim, hdev->channel);
        return HCSR04_TIMEOUT;
    }

    /*
     * Calcula el ancho del pulso ECHO en microsegundos.
     * Cada tick del timer equivale a 1 µs (Prescaler = 71, 72 MHz).
     * Se maneja el caso de overflow del contador de 16 bits.
     */
    uint32_t pulse_us;
    if (hdev->_t_fall >= hdev->_t_rise) {
        pulse_us = hdev->_t_fall - hdev->_t_rise;
    } else {
        /* Overflow: el contador de 16 bits dio vuelta entre flancos */
        pulse_us = (0xFFFFU - hdev->_t_rise) + hdev->_t_fall + 1U;
    }

    /* Convierte tiempo a distancia: d = (t * v_sonido) / 2 */
    float dist = ((float)pulse_us * HCSR04_SOUND_SPEED) / 2.0f;

    /* Satura al máximo reportable para filtrar ruido fuera de rango */
    if (dist > HCSR04_MAX_DISTANCE_CM) {
        dist = HCSR04_MAX_DISTANCE_CM;
    }

    *distance_cm = dist;
    return HCSR04_OK;
}

void HCSR04_CaptureCallback(HCSR04_Handle_t *hdev, TIM_HandleTypeDef *htim)
{
    /* Filtra callbacks de otros timers o canales */
    if (hdev == NULL || htim->Instance != hdev->htim->Instance) {
        return;
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (hdev->_edge_idx == 0U) {
        /*
         * Flanco de SUBIDA: inicio del pulso ECHO.
         * Guarda el valor del registro de captura y cambia la
         * polaridad para esperar el flanco de bajada.
         */
        hdev->_t_rise   = HAL_TIM_ReadCapturedValue(hdev->htim,
                                                      hdev->channel);
        hdev->_edge_idx = 1U;

        __HAL_TIM_SET_CAPTUREPOLARITY(hdev->htim, hdev->channel,
                                       TIM_INPUTCHANNELPOLARITY_FALLING);
    }
    else if (hdev->_edge_idx == 1U) {
        /*
         * Flanco de BAJADA: fin del pulso ECHO.
         * Guarda el valor, detiene Input Capture y despierta la tarea.
         */
        hdev->_t_fall   = HAL_TIM_ReadCapturedValue(hdev->htim,
                                                      hdev->channel);
        hdev->_edge_idx = 2U;

        /* Detiene IC: la próxima medición lo reactivará */
        HAL_TIM_IC_Stop_IT(hdev->htim, hdev->channel);

        /* Resetea polaridad a subida para la próxima llamada */
        __HAL_TIM_SET_CAPTUREPOLARITY(hdev->htim, hdev->channel,
                                       TIM_INPUTCHANNELPOLARITY_RISING);

        /*
         * Libera el semáforo desde la ISR. Si hay una tarea de mayor
         * prioridad esperando, portYIELD_FROM_ISR fuerza el context
         * switch al salir de la interrupción.
         */
        xSemaphoreGiveFromISR(hdev->_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
