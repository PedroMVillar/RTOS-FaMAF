/**
 * @file    ws2812.c
 * @brief   Implementación del driver WS2812B para FreeRTOS en STM32
 *
 * Flujo de una transmisión (WS2812_Refresh):
 *
 *   Tarea FreeRTOS
 *       │
 *       ├─► build_dma_buf()     Convierte pixels[RGB] → dma_buf[CCR]
 *       │   · Aplica brillo (>> 8)
 *       │   · Convierte RGB → GRB (formato del WS2812B)
 *       │   · Mapea coordenadas lógicas → orden físico (serpentina)
 *       │   · Codifica cada bit como T1H o T0H
 *       │   · Agrega RESET_SLOTS con CCR=0 al final
 *       │
 *       ├─► HAL_TIM_PWM_Start_DMA()
 *       │   · DMA escribe dma_buf[i] en CCR en cada evento CC1
 *       │   · La señal PWM en PA6 sigue el protocolo WS2812B
 *       │
 *       └─► xSemaphoreTake()  → TAREA SUSPENDIDA, CPU libre
 *
 *   ISR (DMA TC, luego HAL_TIM_PWM_PulseFinishedCallback)
 *       ├─► HAL_TIM_PWM_Stop_DMA()
 *       └─► xSemaphoreGiveFromISR() → TAREA DESPIERTA
 *
 * Primer período "fantasma":
 *   La primera actualización de CCR por DMA ocurre en el evento CC1,
 *   que se genera cuando counter == CCR_inicial (= 0). Esto significa
 *   que el primer bit del buffer se aplica en el SEGUNDO período del
 *   timer. El primer período envía CCR=0 (1.25 µs en bajo), lo cual
 *   es indetectable por el WS2812B dado que el reset requiere > 50 µs.
 */

#include "ws2812.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Funciones internas                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief  Obtiene la columna física en la cadena de LEDs a partir de
 *         la fila y columna lógica, aplicando el patrón serpentina.
 *
 * Retorna el índice lineal en la cadena de LEDs (0..63).
 */
static inline uint16_t logical_to_physical(uint8_t row, uint8_t col)
{
#if WS2812_SERPENTINE
    /* Filas pares → izquierda a derecha; filas impares → derecha a izquierda */
    uint8_t phys_col = (row & 1U) ? (WS2812_COLS - 1U - col) : col;
#else
    uint8_t phys_col = col;
#endif
    return (uint16_t)row * WS2812_COLS + phys_col;
}

/**
 * @brief  Construye el buffer DMA a partir del buffer de píxeles RGB.
 *
 * Para cada LED (en orden físico de la cadena), escribe 24 valores
 * de CCR correspondientes a los bits G7..G0 R7..R0 B7..B0, seguidos
 * de WS2812_RESET_SLOTS ceros para el reset.
 *
 * El brillo se aplica como: valor_escalado = (valor * brightness) >> 8.
 * Con brightness=255: error máximo de 1 LSB (255*255>>8 = 254), invisible.
 */
static void build_dma_buf(WS2812_Handle_t *hdev)
{
    uint16_t *buf    = hdev->dma_buf;
    uint16_t  bidx   = 0U;
    uint8_t   bright = hdev->brightness;

    for (uint8_t row = 0U; row < WS2812_ROWS; row++) {
        for (uint8_t phys_col = 0U; phys_col < WS2812_COLS; phys_col++) {

            /* Columna lógica que ocupa esta posición física */
#if WS2812_SERPENTINE
            uint8_t col = (row & 1U) ? (WS2812_COLS - 1U - phys_col) : phys_col;
#else
            uint8_t col = phys_col;
#endif
            /* Aplicar brillo y convertir RGB → GRB (orden del WS2812B) */
            uint8_t g = (uint8_t)(((uint16_t)hdev->pixels[row][col][1] * bright) >> 8);
            uint8_t r = (uint8_t)(((uint16_t)hdev->pixels[row][col][0] * bright) >> 8);
            uint8_t b = (uint8_t)(((uint16_t)hdev->pixels[row][col][2] * bright) >> 8);

            /* Empaqueta GRB en un entero de 24 bits, MSB = G7 */
            uint32_t grb = ((uint32_t)g << 16) |
                           ((uint32_t)r <<  8) |
                            (uint32_t)b;

            /* Escribe 24 entradas CCR, bit 23 (MSB) primero */
            for (int8_t bit = 23; bit >= 0; bit--) {
                buf[bidx++] = (grb & (1UL << (uint8_t)bit))
                              ? WS2812_T1H
                              : WS2812_T0H;
            }
        }
    }

    /* Señal de reset: CCR=0 durante RESET_SLOTS períodos (75 µs) */
    memset(&buf[bidx], 0, WS2812_RESET_SLOTS * sizeof(uint16_t));
}

/* ------------------------------------------------------------------ */
/*  API pública                                                        */
/* ------------------------------------------------------------------ */

WS2812_Status_t WS2812_Init(WS2812_Handle_t   *hdev,
                              TIM_HandleTypeDef *htim,
                              uint32_t           channel)
{
    if (hdev == NULL || htim == NULL) {
        return WS2812_ERROR;
    }

    hdev->htim       = htim;
    hdev->channel    = channel;
    hdev->brightness = 255U;

    /* Limpia píxeles y buffer DMA */
    memset(hdev->pixels,  0, sizeof(hdev->pixels));
    memset(hdev->dma_buf, 0, sizeof(hdev->dma_buf));

    hdev->_dma_sem = xSemaphoreCreateBinary();
    if (hdev->_dma_sem == NULL) {
        return WS2812_ERROR;
    }

    return WS2812_OK;
}

/* --- Escritura en buffer de píxeles ------------------------------- */

void WS2812_SetPixel(WS2812_Handle_t *hdev,
                     uint8_t row, uint8_t col,
                     uint8_t r, uint8_t g, uint8_t b)
{
    if (hdev == NULL || row >= WS2812_ROWS || col >= WS2812_COLS) {
        return;
    }
    hdev->pixels[row][col][0] = r;
    hdev->pixels[row][col][1] = g;
    hdev->pixels[row][col][2] = b;
}

void WS2812_SetPixelColor(WS2812_Handle_t *hdev,
                           uint8_t row, uint8_t col,
                           WS2812_Color_t c)
{
    WS2812_SetPixel(hdev, row, col, c.r, c.g, c.b);
}

void WS2812_Fill(WS2812_Handle_t *hdev, uint8_t r, uint8_t g, uint8_t b)
{
    if (hdev == NULL) return;
    for (uint8_t row = 0U; row < WS2812_ROWS; row++) {
        for (uint8_t col = 0U; col < WS2812_COLS; col++) {
            hdev->pixels[row][col][0] = r;
            hdev->pixels[row][col][1] = g;
            hdev->pixels[row][col][2] = b;
        }
    }
}

void WS2812_FillColor(WS2812_Handle_t *hdev, WS2812_Color_t c)
{
    WS2812_Fill(hdev, c.r, c.g, c.b);
}

void WS2812_Clear(WS2812_Handle_t *hdev)
{
    if (hdev == NULL) return;
    memset(hdev->pixels, 0, sizeof(hdev->pixels));
}

void WS2812_SetRow(WS2812_Handle_t *hdev,
                   uint8_t row, uint8_t r, uint8_t g, uint8_t b)
{
    if (hdev == NULL || row >= WS2812_ROWS) return;
    for (uint8_t col = 0U; col < WS2812_COLS; col++) {
        hdev->pixels[row][col][0] = r;
        hdev->pixels[row][col][1] = g;
        hdev->pixels[row][col][2] = b;
    }
}

void WS2812_SetCol(WS2812_Handle_t *hdev,
                   uint8_t col, uint8_t r, uint8_t g, uint8_t b)
{
    if (hdev == NULL || col >= WS2812_COLS) return;
    for (uint8_t row = 0U; row < WS2812_ROWS; row++) {
        hdev->pixels[row][col][0] = r;
        hdev->pixels[row][col][1] = g;
        hdev->pixels[row][col][2] = b;
    }
}

void WS2812_SetBitmap(WS2812_Handle_t  *hdev,
                       const uint8_t    bitmap[8],
                       WS2812_Color_t   fg,
                       WS2812_Color_t   bg)
{
    if (hdev == NULL || bitmap == NULL) return;
    for (uint8_t row = 0U; row < WS2812_ROWS; row++) {
        for (uint8_t col = 0U; col < WS2812_COLS; col++) {
            /* MSB del byte = columna 0 (izquierda) */
            WS2812_Color_t c = (bitmap[row] & (0x80U >> col)) ? fg : bg;
            WS2812_SetPixelColor(hdev, row, col, c);
        }
    }
}

void WS2812_SetBrightness(WS2812_Handle_t *hdev, uint8_t brightness)
{
    if (hdev == NULL) return;
    hdev->brightness = brightness;
}

/* --- Conversión de color ------------------------------------------ */

WS2812_Color_t WS2812_HSV(uint8_t h, uint8_t s, uint8_t v)
{
    WS2812_Color_t c;

    if (s == 0U) {
        /* Sin saturación → gris */
        c.r = c.g = c.b = v;
        return c;
    }

    /*
     * División del círculo de 256 niveles en 6 sectores de 43 unidades.
     * `remainder` es la fracción dentro del sector (0–255).
     */
    uint8_t sector    = h / 43U;
    uint8_t remainder = (uint8_t)((h - (uint16_t)sector * 43U) * 6U);

    uint8_t p = (uint8_t)(((uint16_t)v * (255U - s)) >> 8);
    uint8_t q = (uint8_t)(((uint16_t)v * (255U - (((uint16_t)s * remainder) >> 8))) >> 8);
    uint8_t t = (uint8_t)(((uint16_t)v * (255U - (((uint16_t)s * (255U - remainder)) >> 8))) >> 8);

    switch (sector) {
        case 0:  c.r = v; c.g = t; c.b = p; break;
        case 1:  c.r = q; c.g = v; c.b = p; break;
        case 2:  c.r = p; c.g = v; c.b = t; break;
        case 3:  c.r = p; c.g = q; c.b = v; break;
        case 4:  c.r = t; c.g = p; c.b = v; break;
        default: c.r = v; c.g = p; c.b = q; break;
    }
    return c;
}

WS2812_Color_t WS2812_Lerp(WS2812_Color_t c1, WS2812_Color_t c2, uint8_t t)
{
    WS2812_Color_t out;
    out.r = (uint8_t)(c1.r + (((int16_t)c2.r - c1.r) * t) / 255);
    out.g = (uint8_t)(c1.g + (((int16_t)c2.g - c1.g) * t) / 255);
    out.b = (uint8_t)(c1.b + (((int16_t)c2.b - c1.b) * t) / 255);
    return out;
}

/* --- Transmisión DMA ---------------------------------------------- */

WS2812_Status_t WS2812_Refresh(WS2812_Handle_t *hdev)
{
    if (hdev == NULL || hdev->_dma_sem == NULL) {
        return WS2812_ERROR;
    }

    /* Construye el buffer DMA con los CCR de cada bit */
    build_dma_buf(hdev);

    /* Descarta cualquier give previo no consumido */
    xSemaphoreTake(hdev->_dma_sem, 0);

    /*
     * Inicia el PWM con DMA. La función vincula el DMA al canal CC1 del
     * timer: en cada evento CC1 el DMA copia dma_buf[i] en el registro
     * CCR1, controlando así el ancho del pulso bit a bit.
     *
     * El cast a uint32_t* es requerido por la firma de HAL; el tamaño
     * real de cada elemento lo determina la configuración DMA (HalfWord).
     */
    HAL_TIM_PWM_Start_DMA(hdev->htim, hdev->channel,
                           (uint32_t *)hdev->dma_buf,
                           WS2812_DMA_BUF_SIZE);

    /* Cede la CPU hasta que la ISR complete la transferencia DMA */
    if (xSemaphoreTake(hdev->_dma_sem,
                        pdMS_TO_TICKS(WS2812_TIMEOUT_MS)) == pdFALSE)
    {
        HAL_TIM_PWM_Stop_DMA(hdev->htim, hdev->channel);
        return WS2812_TIMEOUT;
    }

    return WS2812_OK;
}

void WS2812_DMA_Callback(WS2812_Handle_t *hdev, TIM_HandleTypeDef *htim)
{
    if (hdev == NULL || htim->Instance != hdev->htim->Instance) {
        return;
    }

    /*
     * Detiene el DMA y el PWM. En este momento el buffer está en la
     * sección de RESET (CCR=0), así que la línea queda en bajo,
     * lo cual es el estado de reposo correcto para el WS2812B.
     */
    HAL_TIM_PWM_Stop_DMA(hdev->htim, hdev->channel);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(hdev->_dma_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
