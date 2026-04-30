/**
 * @file    ws2812_example.c
 * @brief   Ejemplos de uso del driver WS2812B con FreeRTOS en STM32F103RB
 *
 * Este archivo contiene ejemplos listos para copiar en los archivos del
 * proyecto generado por STM32CubeIDE. NO compilar directamente.
 *
 * Pines usados en este ejemplo:
 *   DATA (DIN): PA6 → TIM3_CH1 (configurado en CubeMX)
 *   (TRIG del HC-SR04 queda en PA8 sin conflicto)
 *
 * =========================================================================
 * SECCIÓN 1 — Includes y handle global
 * (colocar en main.c, junto a los otros includes de CubeMX)
 * =========================================================================
 */

#include "ws2812.h"

/* Handle global — NUNCA declarar como variable local de una tarea:
   el campo dma_buf ocupa ~3.1 KB y no cabe en el stack de una tarea. */
WS2812_Handle_t hmatrix;

/* =========================================================================
 * SECCIÓN 2 — Bitmaps 8x8 predefinidos
 * Cada byte representa una fila. El bit 7 (MSB) es la columna 0 (izquierda).
 * =========================================================================
 */

static const uint8_t BITMAP_HEART[8] = {
    0b00000000,
    0b01100110,
    0b11111111,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
};

static const uint8_t BITMAP_SMILE[8] = {
    0b00111100,
    0b01000010,
    0b10100101,
    0b10000001,
    0b10100101,
    0b10011001,
    0b01000010,
    0b00111100,
};

static const uint8_t BITMAP_ARROW_UP[8] = {
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
};

static const uint8_t BITMAP_CROSS[8] = {
    0b10000001,
    0b01000010,
    0b00100100,
    0b00011000,
    0b00011000,
    0b00100100,
    0b01000010,
    0b10000001,
};

/* Dígitos 0–9 para mostrar números en la matriz (fuente 5x7 centrada) */
static const uint8_t FONT_5x7[10][8] = {
    { 0b00111100, 0b01100110, 0b01100110, 0b01110110, 0b01101110, 0b01100110, 0b00111100, 0b00000000 }, /* 0 */
    { 0b00011000, 0b00111000, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b01111110, 0b00000000 }, /* 1 */
    { 0b00111100, 0b01100110, 0b00000110, 0b00001100, 0b00110000, 0b01100000, 0b01111110, 0b00000000 }, /* 2 */
    { 0b00111100, 0b01100110, 0b00000110, 0b00011100, 0b00000110, 0b01100110, 0b00111100, 0b00000000 }, /* 3 */
    { 0b00001100, 0b00011100, 0b00111100, 0b01101100, 0b01111110, 0b00001100, 0b00001100, 0b00000000 }, /* 4 */
    { 0b01111110, 0b01100000, 0b01111100, 0b00000110, 0b00000110, 0b01100110, 0b00111100, 0b00000000 }, /* 5 */
    { 0b00111100, 0b01100110, 0b01100000, 0b01111100, 0b01100110, 0b01100110, 0b00111100, 0b00000000 }, /* 6 */
    { 0b01111110, 0b01100110, 0b00001100, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00000000 }, /* 7 */
    { 0b00111100, 0b01100110, 0b01100110, 0b00111100, 0b01100110, 0b01100110, 0b00111100, 0b00000000 }, /* 8 */
    { 0b00111100, 0b01100110, 0b01100110, 0b00111110, 0b00000110, 0b01100110, 0b00111100, 0b00000000 }, /* 9 */
};

/* =========================================================================
 * SECCIÓN 3 — Callback de DMA
 * (colocar en main.c, zona "USER CODE BEGIN 4")
 * =========================================================================
 */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    WS2812_DMA_Callback(&hmatrix, htim);
}

/* =========================================================================
 * SECCIÓN 4 — Inicialización
 * (llamar en main(), después de MX_TIM3_Init() y antes de vTaskStartScheduler)
 * =========================================================================
 */
static void Matrix_Init(void)
{
    WS2812_Status_t s = WS2812_Init(&hmatrix, &htim3, TIM_CHANNEL_1);

    if (s != WS2812_OK) {
        /* Fallo de inicialización — bucle de error con LED del Nucleo */
        while (1) {
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            HAL_Delay(200);
        }
    }

    /* Brillo inicial al 30 % para no deslumbrar durante desarrollo */
    WS2812_SetBrightness(&hmatrix, 76);
}

/* =========================================================================
 * SECCIÓN 5 — Funciones de animación reutilizables
 * =========================================================================
 */

/**
 * @brief  Animación arco iris: cada columna tiene un tono HSV distinto
 *         que avanza en el tiempo.
 * @param  cycles   Cantidad de vueltas completas del arco iris.
 * @param  delay_ms Milisegundos entre fotogramas.
 */
static void anim_rainbow(uint16_t cycles, uint32_t delay_ms)
{
    for (uint16_t frame = 0; frame < (uint16_t)(cycles * 256); frame++) {
        for (uint8_t col = 0; col < WS2812_COLS; col++) {
            /* Cada columna desfasa el tono 32/8 = 32 unidades de hue */
            uint8_t hue = (uint8_t)(frame + col * 32U);
            WS2812_Color_t c = WS2812_HSV(hue, 255, 200);
            WS2812_SetCol(&hmatrix, col, c.r, c.g, c.b);
        }
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief  Arco iris diagonal: cada píxel tiene tono según (fila + col).
 * @param  cycles   Ciclos completos.
 * @param  delay_ms Milisegundos entre fotogramas.
 */
static void anim_rainbow_diagonal(uint16_t cycles, uint32_t delay_ms)
{
    for (uint16_t frame = 0; frame < (uint16_t)(cycles * 256); frame++) {
        for (uint8_t row = 0; row < WS2812_ROWS; row++) {
            for (uint8_t col = 0; col < WS2812_COLS; col++) {
                uint8_t hue = (uint8_t)(frame + (row + col) * 16U);
                WS2812_Color_t c = WS2812_HSV(hue, 255, 180);
                WS2812_SetPixelColor(&hmatrix, row, col, c);
            }
        }
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief  Pixel que rebota dentro de la matriz.
 * @param  color    Color del pixel.
 * @param  steps    Cantidad total de pasos de movimiento.
 * @param  delay_ms Milisegundos entre pasos.
 */
static void anim_bouncing_pixel(WS2812_Color_t color, uint16_t steps, uint32_t delay_ms)
{
    int8_t row = 0, col = 0;
    int8_t dr  = 1, dc  = 1;

    for (uint16_t i = 0; i < steps; i++) {
        WS2812_Clear(&hmatrix);
        WS2812_SetPixelColor(&hmatrix, (uint8_t)row, (uint8_t)col, color);
        WS2812_Refresh(&hmatrix);

        row += dr;
        col += dc;

        if (row <= 0 || row >= (int8_t)(WS2812_ROWS - 1)) dr = -dr;
        if (col <= 0 || col >= (int8_t)(WS2812_COLS - 1)) dc = -dc;

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief  Barrido de columnas: enciende una columna a la vez de izq a der.
 * @param  color    Color del barrido.
 * @param  delay_ms Milisegundos por columna.
 */
static void anim_column_sweep(WS2812_Color_t color, uint32_t delay_ms)
{
    for (uint8_t col = 0; col < WS2812_COLS; col++) {
        WS2812_Clear(&hmatrix);
        WS2812_SetCol(&hmatrix, col, color.r, color.g, color.b);
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief  Parpadeo de un bitmap N veces.
 * @param  bitmap   Bitmap de 8 bytes.
 * @param  fg       Color del foreground.
 * @param  times    Cantidad de parpadeos.
 * @param  delay_ms Milisegundos de encendido y apagado.
 */
static void anim_blink_bitmap(const uint8_t bitmap[8], WS2812_Color_t fg,
                               uint8_t times, uint32_t delay_ms)
{
    for (uint8_t i = 0; i < times; i++) {
        WS2812_SetBitmap(&hmatrix, bitmap, fg, WS2812_BLACK);
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        WS2812_Clear(&hmatrix);
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief  Muestra un dígito 0–9 con la fuente 5x7 interna.
 */
static void show_digit(uint8_t digit, WS2812_Color_t fg)
{
    if (digit > 9U) return;
    WS2812_SetBitmap(&hmatrix, FONT_5x7[digit], fg, WS2812_BLACK);
    WS2812_Refresh(&hmatrix);
}

/**
 * @brief  Cuenta regresiva de 9 a 0 con colores degradados (verde → rojo).
 * @param  delay_ms  Tiempo en ms que se muestra cada dígito.
 */
static void anim_countdown(uint32_t delay_ms)
{
    for (int8_t d = 9; d >= 0; d--) {
        /* Interpola de verde (d=9) a rojo (d=0) */
        uint8_t t = (uint8_t)((9U - (uint8_t)d) * 28U);   /* 0..252 */
        WS2812_Color_t c = WS2812_Lerp(WS2812_GREEN, WS2812_RED, t);
        show_digit((uint8_t)d, c);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief  Efecto de onda: brillo sinusoidal por fila usando aproximación entera.
 *         No requiere libm ni FPU.
 * @param  cycles     Ciclos de onda a reproducir.
 * @param  delay_ms   Milisegundos entre fotogramas.
 */
static void anim_wave(WS2812_Color_t color, uint16_t cycles, uint32_t delay_ms)
{
    /* Tabla de seno aproximado (cuarto de onda, 0–90° en 8 pasos) */
    static const uint8_t sine8[8] = { 0, 50, 100, 148, 187, 220, 244, 255 };

    for (uint16_t frame = 0; frame < (uint16_t)(cycles * 16); frame++) {
        for (uint8_t row = 0; row < WS2812_ROWS; row++) {
            /* Índice en la tabla para esta fila y fotograma */
            uint8_t idx  = (frame + row * 2U) & 0x0FU;  /* 0..15 */
            /* Simetría del seno: segunda mitad es espejo de la primera */
            uint8_t sine = (idx < 8U) ? sine8[idx] : sine8[15U - idx];
            uint8_t r = (uint8_t)(((uint16_t)color.r * sine) >> 8);
            uint8_t g = (uint8_t)(((uint16_t)color.g * sine) >> 8);
            uint8_t b = (uint8_t)(((uint16_t)color.b * sine) >> 8);
            WS2812_SetRow(&hmatrix, row, r, g, b);
        }
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* =========================================================================
 * SECCIÓN 6 — Tarea FreeRTOS principal de la matriz
 * (definición de la tarea que encadena los efectos)
 * =========================================================================
 */

#define MATRIX_TASK_STACK  512U   /* palabras — el handle global consume RAM
                                     adicional, el stack solo ejecuta código */
#define MATRIX_TASK_PRIO   2U

void vMatrixTask(void *pvParameters)
{
    (void)pvParameters;

    /* Breve apagado inicial para asegurar reset de los LEDs */
    WS2812_Clear(&hmatrix);
    WS2812_Refresh(&hmatrix);
    vTaskDelay(pdMS_TO_TICKS(100));

    for (;;)
    {
        /* 1. Corazón parpadeando en rojo */
        anim_blink_bitmap(BITMAP_HEART, WS2812_RED, 5, 400);

        /* 2. Sonrisa en amarillo */
        WS2812_SetBrightness(&hmatrix, 80);
        WS2812_SetBitmap(&hmatrix, BITMAP_SMILE, WS2812_YELLOW, WS2812_BLACK);
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* 3. Arco iris por columnas */
        WS2812_SetBrightness(&hmatrix, 100);
        anim_rainbow(2, 30);

        /* 4. Arco iris diagonal */
        anim_rainbow_diagonal(2, 40);

        /* 5. Pixel que rebota en verde */
        WS2812_SetBrightness(&hmatrix, 120);
        anim_bouncing_pixel(WS2812_GREEN, 80, 60);

        /* 6. Barrido de columnas en azul */
        WS2812_SetBrightness(&hmatrix, 150);
        for (uint8_t i = 0; i < 3; i++) {
            anim_column_sweep(WS2812_BLUE, 120);
        }

        /* 7. Onda cian */
        anim_wave(WS2812_CYAN, 4, 50);

        /* 8. Cuenta regresiva 9..0 */
        WS2812_SetBrightness(&hmatrix, 100);
        anim_countdown(600);

        /* 9. Apagado con fundido hacia negro pixel a pixel */
        for (int16_t bright = 100; bright >= 0; bright -= 5) {
            WS2812_SetBrightness(&hmatrix, (uint8_t)bright);
            WS2812_Fill(&hmatrix, 255, 255, 255);
            WS2812_Refresh(&hmatrix);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        WS2812_Clear(&hmatrix);
        WS2812_Refresh(&hmatrix);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Restaura brillo para el siguiente ciclo */
        WS2812_SetBrightness(&hmatrix, 76);
    }
}

/* =========================================================================
 * SECCIÓN 7 — Fragmento para main() (referencia)
 *
 * Copiar dentro de int main(void) en la zona USER CODE BEGIN 2:
 * =========================================================================
 *
 *   Matrix_Init();
 *
 *   xTaskCreate(vMatrixTask,
 *               "Matrix",
 *               MATRIX_TASK_STACK,
 *               NULL,
 *               MATRIX_TASK_PRIO,
 *               NULL);
 *
 *   vTaskStartScheduler();
 *
 * =========================================================================
 * SECCIÓN 8 — Uso simultáneo con el driver HC-SR04
 *
 * Ambos drivers pueden correr en el mismo proyecto. La única restricción
 * es que el HC-SR04 usa TIM2 y el WS2812 usa TIM3, sin conflicto.
 * Si querés pintar la matriz según la distancia medida:
 * =========================================================================
 *
 *   void vSonarMatrixTask(void *pvParameters) {
 *       float dist_cm;
 *       TickType_t xLastWake = xTaskGetTickCount();
 *
 *       for (;;) {
 *           HCSR04_Measure(&hsonar, &dist_cm);
 *
 *           // Mapa: 0 cm = rojo, 40 cm = verde, > 40 cm = azul
 *           WS2812_Color_t c;
 *           if (dist_cm < 10.0f) {
 *               c = WS2812_RED;
 *           } else if (dist_cm < 40.0f) {
 *               uint8_t t = (uint8_t)((dist_cm - 10.0f) / 30.0f * 255.0f);
 *               c = WS2812_Lerp(WS2812_RED, WS2812_GREEN, t);
 *           } else {
 *               c = WS2812_BLUE;
 *           }
 *
 *           WS2812_FillColor(&hmatrix, c);
 *           WS2812_Refresh(&hmatrix);
 *
 *           vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(200));
 *       }
 *   }
 */
