/**
 * @file    joystick_yxa197.c
 * @brief   Implementación del driver joystick YXA197 para FreeRTOS en STM32
 *
 * ---- Principio de lectura ADC (polling sin DMA) ----------------------------
 * Cada ciclo de tarea llama a prv_adc_read() dos veces: una para el eje X y
 * otra para el eje Y. Cada llamada:
 *   1. Reconfigura el canal activo con HAL_ADC_ConfigChannel.
 *   2. Inicia una conversión única con HAL_ADC_Start.
 *   3. Espera el resultado con HAL_ADC_PollForConversion (timeout: 10 ms).
 *   4. Lee el resultado y detiene el ADC con HAL_ADC_Stop.
 *
 * A 12 MHz de reloj ADC y 28.5 ciclos de muestreo, cada conversión tarda
 * ~3.4 µs. Dos conversiones por ciclo: ~7 µs, despreciable frente a los 20 ms
 * del período de polling. No se requiere DMA ni modo Scan en CubeMX.
 *
 * ---- Anti-rebote de botones (histéresis por contador) ----------------------
 * Se mantiene un contador [0, JOYSTICK_DEBOUNCE_COUNT] por botón:
 *   - Botón físicamente presionado → incrementar contador (hasta el límite).
 *   - Botón físicamente suelto     → decrementar contador (hasta 0).
 * El estado lógico estable cambia únicamente cuando el contador llega al
 * máximo (presionado) o cae a 0 (suelto). Con DEBOUNCE_COUNT=3 y período
 * 20 ms, el debounce efectivo es 60 ms, suficiente para pulsadores mecánicos.
 *
 * ---- Conversión eje analógico → dirección ----------------------------------
 * Se calcula la desviación con signo desde el centro:
 *   rx = x_raw − CENTER    → positivo = derecha
 *   ry = CENTER − y_raw    → positivo = arriba (Y hardware decreciente al subir)
 * Si los flags invert_x / invert_y están activos, se niegan los valores.
 * La dirección se determina comparando |rx| e |ry| contra el umbral configurado.
 *
 * ---- Seguridad entre tareas ------------------------------------------------
 * Los campos volatile (s_x_raw, s_y_raw, s_current_dir, s_btn_state[]) son
 * escritos exclusivamente por vJoystickTask y leídos por las funciones getter.
 * En ARM Cortex-M, lecturas y escrituras de uint8_t y uint16_t correctamente
 * alineados son atómicas (instrucción LDRH/STRH de un solo acceso al bus),
 * por lo que no se requiere sección crítica para estas lecturas de solo
 * monitoreo. La cola de eventos usa la propia sincronización de FreeRTOS.
 */

#include "joystick_yxa197.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Estado interno (singleton — un joystick por sistema)               */
/* ------------------------------------------------------------------ */

static JoystickConfig_t s_cfg;

/* Últimos valores leídos — escritos solo por vJoystickTask */
static volatile uint16_t s_x_raw       = JOYSTICK_CENTER_VALUE;
static volatile uint16_t s_y_raw       = JOYSTICK_CENTER_VALUE;
static volatile JoystickDirection_t s_current_dir = JOY_DIR_CENTER;
static volatile uint8_t s_btn_state[JOYSTICK_MAX_BUTTONS];

/* Estado privado de la tarea (no necesita volatile: solo lo accede la tarea) */
static uint8_t             s_debounce_cnt[JOYSTICK_MAX_BUTTONS];
static JoystickDirection_t s_prev_dir = JOY_DIR_CENTER;
static uint8_t             s_prev_btn[JOYSTICK_MAX_BUTTONS];

/* ------------------------------------------------------------------ */
/*  Funciones privadas                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief  Configura el canal ADC indicado y realiza una conversión simple.
 *
 * Reconfigura dinámicamente el canal antes de cada lectura. CubeMX debe
 * haber inicializado el ADC en modo Single Conversion (no Continuous, no Scan).
 *
 * @param  hadc     Handle del ADC (ej. &hadc1).
 * @param  channel  Canal a leer (ej. ADC_CHANNEL_0, ADC_CHANNEL_1).
 * @return Resultado 12 bits [0–4095]. Retorna JOYSTICK_CENTER_VALUE si
 *         HAL_ADC_PollForConversion expira (sensor desconectado / ruido severo).
 */
static uint16_t prv_adc_read(ADC_HandleTypeDef *hadc, uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = channel;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = JOYSTICK_ADC_SAMPLETIME;
    HAL_ADC_ConfigChannel(hadc, &sConfig);

    HAL_ADC_Start(hadc);
    if (HAL_ADC_PollForConversion(hadc, JOYSTICK_ADC_TIMEOUT_MS) != HAL_OK) {
        /* Conversión no completada en tiempo: retornar centro como valor seguro */
        HAL_ADC_Stop(hadc);
        return (uint16_t)JOYSTICK_CENTER_VALUE;
    }
    uint16_t val = (uint16_t)HAL_ADC_GetValue(hadc);
    HAL_ADC_Stop(hadc);
    return val;
}

/**
 * @brief  Calcula la JoystickDirection_t a partir de lecturas ADC crudas.
 *
 * Convención por defecto (sin inversión):
 *   x_raw alta → DERECHA  |  x_raw baja → IZQUIERDA
 *   y_raw baja → ARRIBA   |  y_raw alta → ABAJO
 * (Típico en módulos de joystick con potenciómetro: Y disminuye al subir.)
 * Si la orientación física es opuesta, usar invert_x/invert_y en la config.
 */
static JoystickDirection_t prv_calc_direction(uint16_t x, uint16_t y)
{
    /* Desviación con signo desde el punto central */
    int32_t rx = (int32_t)x - (int32_t)JOYSTICK_CENTER_VALUE; /* + = derecha  */
    int32_t ry = (int32_t)JOYSTICK_CENTER_VALUE - (int32_t)y; /* + = arriba   */

    if (s_cfg.invert_x) rx = -rx;
    if (s_cfg.invert_y) ry = -ry;

    int32_t thr = (int32_t)s_cfg.threshold;

    uint8_t right = (rx >  thr);
    uint8_t left  = (rx < -thr);
    uint8_t up    = (ry >  thr);
    uint8_t down  = (ry < -thr);

    /* Diagonales tienen prioridad sobre cardinales */
    if (up   && right) return JOY_DIR_UP_RIGHT;
    if (up   && left)  return JOY_DIR_UP_LEFT;
    if (down && right) return JOY_DIR_DOWN_RIGHT;
    if (down && left)  return JOY_DIR_DOWN_LEFT;
    if (up)            return JOY_DIR_UP;
    if (down)          return JOY_DIR_DOWN;
    if (right)         return JOY_DIR_RIGHT;
    if (left)          return JOY_DIR_LEFT;
    return JOY_DIR_CENTER;
}

/* ------------------------------------------------------------------ */
/*  Tarea interna FreeRTOS                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief  Tarea de polling del joystick (período fijo con vTaskDelayUntil).
 *
 * Ciclo de cada iteración:
 *   1. Leer eje X e Y via ADC (reconfigurando canal entre lecturas).
 *   2. Calcular dirección; publicar evento si cambió respecto al ciclo anterior.
 *   3. Para cada botón configurado: leer GPIO, actualizar contador de debounce
 *      y publicar evento de PRESS o RELEASE al detectar transición estable.
 *   4. Dormir hasta el siguiente período con vTaskDelayUntil (sin deriva).
 */
static void vJoystickTask(void *pvParameters)
{
    (void)pvParameters;

    TickType_t       xLastWake = xTaskGetTickCount();
    const TickType_t xPeriod   = pdMS_TO_TICKS(s_cfg.poll_period_ms);
    JoystickEvent_t  event;
    uint8_t          i;

    for (;;)
    {
        /* ---- 1. Lectura de ejes analógicos ------------------------------ */
        uint16_t x = prv_adc_read(s_cfg.hadc, s_cfg.x_channel);
        uint16_t y = prv_adc_read(s_cfg.hadc, s_cfg.y_channel);

        s_x_raw = x;
        s_y_raw = y;

        /* ---- 2. Dirección ----------------------------------------------- */
        JoystickDirection_t dir = prv_calc_direction(x, y);
        s_current_dir = dir;

        if (dir != s_prev_dir)
        {
            event.type      = JOY_EVENT_DIRECTION_CHANGE;
            event.direction = dir;
            event.x_raw     = x;
            event.y_raw     = y;
            /* Envío no bloqueante: si la cola está llena se descarta el evento */
            xQueueSend(s_cfg.event_queue, &event, 0U);
            s_prev_dir = dir;
        }

        /* ---- 3. Botones con anti-rebote ---------------------------------- */
        for (i = 0; i < s_cfg.button_count && i < JOYSTICK_MAX_BUTTONS; i++)
        {
            GPIO_PinState pin = HAL_GPIO_ReadPin(s_cfg.buttons[i].port,
                                                  s_cfg.buttons[i].pin);
            uint8_t raw_pressed = (pin == s_cfg.buttons[i].active_state) ? 1U : 0U;

            /*
             * Histéresis: el contador sube al presionar y baja al soltar.
             * El estado lógico solo cambia en los extremos del contador.
             */
            if (raw_pressed)
            {
                if (s_debounce_cnt[i] < JOYSTICK_DEBOUNCE_COUNT)
                    s_debounce_cnt[i]++;
            }
            else
            {
                if (s_debounce_cnt[i] > 0U)
                    s_debounce_cnt[i]--;
            }

            uint8_t stable = (s_debounce_cnt[i] == JOYSTICK_DEBOUNCE_COUNT) ? 1U : 0U;
            s_btn_state[i] = stable;

            if (stable != s_prev_btn[i])
            {
                event.type   = (stable != 0U) ? JOY_EVENT_BUTTON_PRESS
                                               : JOY_EVENT_BUTTON_RELEASE;
                event.button = (JoystickButtonId_t)i;
                event.x_raw  = x;
                event.y_raw  = y;
                xQueueSend(s_cfg.event_queue, &event, 0U);
                s_prev_btn[i] = stable;
            }
        }

        /* ---- 4. Período fijo -------------------------------------------- */
        vTaskDelayUntil(&xLastWake, xPeriod);
    }
}

/* ------------------------------------------------------------------ */
/*  API pública                                                        */
/* ------------------------------------------------------------------ */

void Joystick_GetDefaultConfig(JoystickConfig_t *config)
{
    memset(config, 0, sizeof(*config));
    config->threshold      = JOYSTICK_THRESHOLD_DEFAULT;
    config->task_priority  = 2U;
    config->poll_period_ms = JOYSTICK_POLL_PERIOD_MS;
    /* invert_x e invert_y quedan en 0 (sin inversión) */
}

BaseType_t Joystick_Init(const JoystickConfig_t *config)
{
    if (config == NULL || config->hadc == NULL || config->event_queue == NULL) {
        return pdFAIL;
    }
    if (config->button_count == 0U || config->button_count > JOYSTICK_MAX_BUTTONS) {
        return pdFAIL;
    }
    if (config->threshold == 0U || config->poll_period_ms == 0U) {
        return pdFAIL;
    }

    /* Copia la configuración para independencia del scope del llamador */
    memcpy(&s_cfg, config, sizeof(s_cfg));

    /* Inicializa el estado interno antes de arrancar la tarea */
    s_x_raw       = (uint16_t)JOYSTICK_CENTER_VALUE;
    s_y_raw       = (uint16_t)JOYSTICK_CENTER_VALUE;
    s_current_dir = JOY_DIR_CENTER;
    s_prev_dir    = JOY_DIR_CENTER;

    for (uint8_t i = 0U; i < JOYSTICK_MAX_BUTTONS; i++) {
        s_btn_state[i]    = 0U;
        s_debounce_cnt[i] = 0U;
        s_prev_btn[i]     = 0U;
    }

    /* Crea la tarea interna — requiere heap disponible en FreeRTOS */
    return xTaskCreate(vJoystickTask,
                       "Joystick",
                       JOYSTICK_TASK_STACK_WORDS,
                       NULL,
                       s_cfg.task_priority,
                       NULL);
}

JoystickDirection_t Joystick_GetDirection(void)
{
    return s_current_dir;
}

void Joystick_GetRaw(uint16_t *x, uint16_t *y)
{
    if (x != NULL) { *x = s_x_raw; }
    if (y != NULL) { *y = s_y_raw; }
}

uint8_t Joystick_IsButtonPressed(JoystickButtonId_t btn)
{
    if ((uint8_t)btn >= JOYSTICK_MAX_BUTTONS) {
        return 0U;
    }
    return s_btn_state[(uint8_t)btn];
}

const char *Joystick_DirectionToString(JoystickDirection_t dir)
{
    switch (dir) {
        case JOY_DIR_CENTER:     return "CENTRO";
        case JOY_DIR_UP:         return "ARRIBA";
        case JOY_DIR_DOWN:       return "ABAJO";
        case JOY_DIR_LEFT:       return "IZQUIERDA";
        case JOY_DIR_RIGHT:      return "DERECHA";
        case JOY_DIR_UP_LEFT:    return "ARRIBA-IZQ";
        case JOY_DIR_UP_RIGHT:   return "ARRIBA-DER";
        case JOY_DIR_DOWN_LEFT:  return "ABAJO-IZQ";
        case JOY_DIR_DOWN_RIGHT: return "ABAJO-DER";
        default:                 return "DESCONOCIDO";
    }
}
