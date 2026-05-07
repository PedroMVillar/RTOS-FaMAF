/**
 * @file    ws2812.h
 * @brief   Driver para matriz NeoPixel 8x8 WS2812B en STM32 con FreeRTOS
 *
 * Principio de funcionamiento del WS2812B:
 *   El WS2812B usa un protocolo serie de un solo hilo. Cada LED recibe
 *   24 bits en orden GRB (Green-Red-Blue), MSB primero. Los bits se
 *   codifican por el ancho del pulso en alto dentro de un período de
 *   1.25 µs:
 *       Bit '1': T1H = 0.8 µs alto,  T1L = 0.45 µs bajo
 *       Bit '0': T0H = 0.4 µs alto,  T0L = 0.85 µs bajo
 *       Reset  : > 50 µs en bajo
 *
 * Estrategia de generación de señal (Timer PWM + DMA):
 *   Se usa un timer en modo PWM cuyo registro CCR se actualiza por DMA
 *   en cada período. El buffer DMA contiene el valor de CCR (= ancho
 *   del pulso en alto) para cada bit de cada LED. Al completar la
 *   transferencia DMA, la ISR libera un semáforo FreeRTOS que despierta
 *   la tarea que llamó a WS2812_Refresh(). El CPU no realiza busy-wait
 *   durante la transmisión.
 *
 * Configuración del timer en STM32CubeMX (TIM2, PA1):
 *   - Prescaler  : 0  (72 MHz sin división)
 *   - ARR        : 89 (período = 90/72MHz ≈ 1.25 µs)
 *   - Channel 2  : PWM Generation CH2, Mode 1, High Polarity
 *   - DMA        : TIM2_CH2 → DMA1_Channel7, Mem→Periph,
 *                  HalfWord/HalfWord, Normal, Increment Memory
 *   - NVIC DMA   : DMA1 channel7 interrupt habilitado, prioridad 5
 *
 * Nota: PA1 = TIM2_CH2. TIM2 es incompatible con el driver HC-SR04
 *   (que también usa TIM2 para Input Capture). No usar ambos juntos.
 *
 * Uso básico:
 * @code
 *   WS2812_Handle_t hmatrix;
 *   WS2812_Init(&hmatrix, &htim2, TIM_CHANNEL_2);
 *
 *   // En HAL_TIM_PWM_PulseFinishedCallback:
 *   WS2812_DMA_Callback(&hmatrix, htim);
 *
 *   // En la tarea FreeRTOS:
 *   WS2812_SetPixel(&hmatrix, 0, 0, 255, 0, 0);   // pixel (0,0) rojo
 *   WS2812_Refresh(&hmatrix);                       // envía a los LEDs
 * @endcode
 *
 * Nota sobre el cableado serpentina (más común en matrices 8x8):
 *   Fila 0: LED 0..7   (izq → der)
 *   Fila 1: LED 8..15  (der → izq)
 *   Fila 2: LED 16..23 (izq → der)  ... y así sucesivamente.
 *   La API siempre usa coordenadas lógicas (fila, col) con col=0 = izq.
 *   Si tu matriz no es serpentina, ajustar WS2812_SERPENTINE en 0.
 */

#ifndef WS2812_H
#define WS2812_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "tim.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ------------------------------------------------------------------ */
/*  Configuración de la matriz — ajustar según hardware               */
/* ------------------------------------------------------------------ */

#define WS2812_ROWS         8U
#define WS2812_COLS         8U
#define WS2812_NUM_LEDS     (WS2812_ROWS * WS2812_COLS)   /* 64 LEDs  */

/** 1 = cableado serpentina (más común), 0 = filas todas izq→der */
#define WS2812_SERPENTINE   1U

/* ------------------------------------------------------------------ */
/*  Constantes de temporización — NO modificar si se usa 72 MHz       */
/*                                                                     */
/*  Timer @ 72 MHz, ARR=89 → período = 1.2500 µs                     */
/*  T1H: CCR=58 → 0.8055 µs (+6 ns sobre spec, dentro de ±150 ns)   */
/*  T0H: CCR=29 → 0.4027 µs (+2 ns sobre spec, dentro de ±150 ns)   */
/* ------------------------------------------------------------------ */

#define WS2812_TIM_ARR          89U
#define WS2812_T1H              58U
#define WS2812_T0H              29U

/** Slots de reset al final del buffer: 60 × 1.25 µs = 75 µs (≥ 50 µs) */
#define WS2812_RESET_SLOTS      60U

/** Tamaño total del buffer DMA en halfwords */
#define WS2812_DMA_BUF_SIZE     (WS2812_NUM_LEDS * 24U + WS2812_RESET_SLOTS)

/** Timeout máximo de espera en WS2812_Refresh() */
#define WS2812_TIMEOUT_MS       50U

/* ------------------------------------------------------------------ */
/*  Tipos públicos                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    WS2812_OK      = 0,
    WS2812_ERROR   = 1,
    WS2812_TIMEOUT = 2
} WS2812_Status_t;

/** Color RGB empaquetado. Uso: WS2812_SetPixelColor(&h, r, c, WS2812_RED) */
typedef struct {
    uint8_t r, g, b;
} WS2812_Color_t;

/* Colores predefinidos */
#define WS2812_BLACK    ((WS2812_Color_t){ 0,   0,   0   })
#define WS2812_WHITE    ((WS2812_Color_t){ 255, 255, 255 })
#define WS2812_RED      ((WS2812_Color_t){ 255, 0,   0   })
#define WS2812_GREEN    ((WS2812_Color_t){ 0,   255, 0   })
#define WS2812_BLUE     ((WS2812_Color_t){ 0,   0,   255 })
#define WS2812_YELLOW   ((WS2812_Color_t){ 255, 255, 0   })
#define WS2812_CYAN     ((WS2812_Color_t){ 0,   255, 255 })
#define WS2812_MAGENTA  ((WS2812_Color_t){ 255, 0,   255 })
#define WS2812_ORANGE   ((WS2812_Color_t){ 255, 128, 0   })

/**
 * @brief Handle principal del driver.
 *
 * Declarar como variable GLOBAL (no en stack de tarea): el campo
 * dma_buf ocupa ~3.1 KB y no cabe en el stack de una tarea típica.
 *
 * Los campos prefijados con '_' son internos.
 */
typedef struct {
    TIM_HandleTypeDef *htim;          /**< Timer con canal PWM+DMA.   */
    uint32_t           channel;       /**< Canal del timer.           */
    uint8_t            brightness;    /**< Brillo global (0–255).     */

    /** Buffer de píxeles en formato RGB lógico [fila][col][R,G,B]. */
    uint8_t  pixels[WS2812_ROWS][WS2812_COLS][3];

    /** Buffer DMA con los valores CCR de cada bit. Solo escritura interna. */
    uint16_t dma_buf[WS2812_DMA_BUF_SIZE];

    SemaphoreHandle_t _dma_sem;       /**< Semáforo DMA→tarea.        */
} WS2812_Handle_t;

/* ------------------------------------------------------------------ */
/*  API pública                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief  Inicializa el handle, el semáforo y apaga todos los LEDs.
 *
 * @param  hdev    Puntero al handle (variable global).
 * @param  htim    Timer HAL configurado con PWM + DMA en el canal.
 * @param  channel Canal del timer (ej. TIM_CHANNEL_1).
 * @retval WS2812_OK o WS2812_ERROR si el semáforo no pudo crearse.
 */
WS2812_Status_t WS2812_Init(WS2812_Handle_t   *hdev,
                              TIM_HandleTypeDef *htim,
                              uint32_t           channel);

/* --- Escritura en buffer de píxeles (sin enviar aún a los LEDs) --- */

/**
 * @brief  Establece el color de un píxel en el buffer interno.
 * @param  row, col  Coordenadas lógicas (0..7, 0..7). col=0 = izquierda.
 * @param  r, g, b   Componentes RGB (0–255 cada una).
 */
void WS2812_SetPixel(WS2812_Handle_t *hdev,
                     uint8_t row, uint8_t col,
                     uint8_t r, uint8_t g, uint8_t b);

/** Versión de WS2812_SetPixel que acepta WS2812_Color_t. */
void WS2812_SetPixelColor(WS2812_Handle_t *hdev,
                           uint8_t row, uint8_t col,
                           WS2812_Color_t c);

/** Rellena toda la matriz con un color RGB. */
void WS2812_Fill(WS2812_Handle_t *hdev, uint8_t r, uint8_t g, uint8_t b);

/** Versión de WS2812_Fill que acepta WS2812_Color_t. */
void WS2812_FillColor(WS2812_Handle_t *hdev, WS2812_Color_t c);

/** Apaga todos los LEDs (equivale a Fill(0,0,0)). */
void WS2812_Clear(WS2812_Handle_t *hdev);

/** Pinta una fila completa con el color especificado. */
void WS2812_SetRow(WS2812_Handle_t *hdev,
                   uint8_t row, uint8_t r, uint8_t g, uint8_t b);

/** Pinta una columna completa con el color especificado. */
void WS2812_SetCol(WS2812_Handle_t *hdev,
                   uint8_t col, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief  Dibuja un bitmap de 8x8 sobre la matriz.
 *
 * Cada byte de bitmap[row] representa una fila: el bit 7 (MSB) es la
 * columna 0 (izquierda). Bit en 1 → color fg, bit en 0 → color bg.
 *
 * @param  bitmap  Array de 8 bytes (un byte por fila).
 * @param  fg      Color de los bits en 1.
 * @param  bg      Color de los bits en 0.
 */
void WS2812_SetBitmap(WS2812_Handle_t  *hdev,
                       const uint8_t    bitmap[8],
                       WS2812_Color_t   fg,
                       WS2812_Color_t   bg);

/**
 * @brief  Ajusta el brillo global de la matriz (0 = apagado, 255 = máximo).
 *
 * No modifica el buffer de píxeles; el factor se aplica al construir el
 * buffer DMA en WS2812_Refresh(). Permite cambiar el brillo sin
 * recalcular todos los colores.
 */
void WS2812_SetBrightness(WS2812_Handle_t *hdev, uint8_t brightness);

/* --- Conversión de color ------------------------------------------ */

/**
 * @brief  Convierte HSV a RGB (formato WS2812_Color_t).
 *
 * h: 0–255 (mapeado a 0–360°), s: 0–255, v: 0–255.
 * Útil para arco iris y efectos de color suaves.
 */
WS2812_Color_t WS2812_HSV(uint8_t h, uint8_t s, uint8_t v);

/**
 * @brief  Mezcla linealmente dos colores con factor t (0=c1, 255=c2).
 */
WS2812_Color_t WS2812_Lerp(WS2812_Color_t c1, WS2812_Color_t c2, uint8_t t);

/* --- Transmisión a los LEDs --------------------------------------- */

/**
 * @brief  Envía el buffer de píxeles a la cadena de LEDs via DMA.
 *
 * Construye el buffer DMA interno, inicia la transferencia PWM+DMA y
 * suspende la tarea llamante hasta que la ISR la despierte (o hasta
 * que expire el timeout de WS2812_TIMEOUT_MS ms).
 *
 * @note   Solo llamar desde una tarea FreeRTOS, nunca desde una ISR.
 *
 * @retval WS2812_OK      si la transmisión completó.
 *         WS2812_TIMEOUT si el DMA no completó en tiempo.
 *         WS2812_ERROR   si hdev es NULL.
 */
WS2812_Status_t WS2812_Refresh(WS2812_Handle_t *hdev);

/**
 * @brief  Callback que debe llamarse desde HAL_TIM_PWM_PulseFinishedCallback.
 *
 * Verifica que el htim corresponda al del handle. Si coincide, detiene
 * el DMA y libera el semáforo que despierta la tarea en Refresh().
 *
 * Ejemplo:
 * @code
 *   void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim) {
 *       WS2812_DMA_Callback(&hmatrix, htim);
 *   }
 * @endcode
 */
void WS2812_DMA_Callback(WS2812_Handle_t *hdev, TIM_HandleTypeDef *htim);

#ifdef __cplusplus
}
#endif

#endif /* WS2812_H */
