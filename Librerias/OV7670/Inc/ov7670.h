/**
 * @file    ov7670.h
 * @brief   OV7670 camera driver for STM32 Nucleo F103RB + FreeRTOS.
 *
 * =========================================================================
 * Architecture overview
 * =========================================================================
 *
 * Pixel capture uses TIM2 (Input Capture on PCLK) + DMA1_CH5 to transfer
 * one horizontal line at a time from the GPIO data port into a line buffer,
 * with zero CPU involvement during transfer.
 *
 *   PCLK ──► PA0 (TIM2_CH1 Input Capture) ──► DMA1_CH5 request
 *   DMA1_CH5: src = GPIOC->IDR (D0–D7 on PC0–PC7)
 *             dst = line_buffer[i], count = line_bytes
 *
 * COM10_PCLK_GATE_HREF is set in the sensor so PCLK only pulses during
 * active horizontal data (HREF high). This means each DMA transfer count
 * equals exactly width × bytes_per_pixel valid bytes — no blanking garbage.
 *
 * VSYNC (PA2, EXTI2, falling edge) signals start of a new frame.
 * After every DMA_TC interrupt, the driver either:
 *   - Calls line_callback and re-arms DMA for the next line, or
 *   - Signals the FreeRTOS semaphore on the last line (frame complete).
 *
 * =========================================================================
 * Default pin mapping (change in OV7670_Config if needed)
 * =========================================================================
 *
 *  Signal     MCU pin   Peripheral       CubeMX mode
 *  ---------  --------  ---------------  --------------------------------
 *  D0–D7      PC0–PC7   GPIO input       GPIO_Input, No pull
 *  PCLK       PA0       TIM2_CH1         TIM2 Input Capture
 *  VSYNC      PA2       EXTI2            GPIO_EXTI, falling edge
 *  HREF       PA3       GPIO input       GPIO_Input (optional monitor)
 *  XCLK       PA8       MCO              RCC → MCO = HSE (8 MHz)
 *  RESET      PB0       GPIO output      GPIO_Output, push-pull, init HIGH
 *  PWDN       PB1       GPIO output      GPIO_Output, push-pull, init LOW
 *  SCCB_SCL   PB6       GPIO output OD   GPIO_Output, Open-Drain, No pull
 *  SCCB_SDA   PB7       GPIO output OD   GPIO_Output, Open-Drain, No pull
 *
 *  External 4.7 kΩ pull-ups to 3.3 V are required on PB6 and PB7.
 *
 * =========================================================================
 * RAM budget for OV7670_FRAME_QQVGA_RGB565
 * =========================================================================
 *
 *  STM32F103RB has 20 KB SRAM total.
 *  Line buffer (QQVGA RGB565): 160 × 2 = 320 bytes  ← allocated by library
 *  Full frame buffer: user-supplied or NULL (line-callback mode only).
 *  Typical FreeRTOS + stack overhead: ~6–8 KB.
 *  Recommendation: use line_callback to process data on-the-fly without
 *  storing a complete frame, or keep a 9 600-byte buffer for QQQVGA.
 */

#ifndef OV7670_H
#define OV7670_H

#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "ov7670_sccb.h"
#include "ov7670_regs.h"

/* =========================================================================
 * Public types
 * ========================================================================= */

/** Pixel output format */
typedef enum {
    OV7670_FMT_RGB565  = 0,  /**< 2 bytes/pixel, big-endian RGB565 */
    OV7670_FMT_YUV422  = 1,  /**< 2 bytes/pixel, YUYV interleaved  */
} OV7670_Format;

/** Output resolution */
typedef enum {
    OV7670_RES_QQVGA = 0,   /**< 160 × 120 pixels */
    OV7670_RES_QVGA  = 1,   /**< 320 × 240 pixels — needs 150 KB for full frame, use line_callback */
} OV7670_Resolution;

/**
 * @brief  Callback invoked from DMA interrupt context after each line.
 *
 * @param  line_data  Pointer to raw line bytes in the line_buffer.
 *                    For RGB565: pairs of bytes per pixel (big-endian).
 *                    For YUV422: YUYV interleaved.
 * @param  line_num   Zero-based line index within the current frame.
 * @param  width      Image width in pixels.
 *
 * @note   Keep this callback short. It runs inside the DMA IRQ handler.
 *         Use task notifications or queues to forward data to a task.
 */
typedef void (*OV7670_LineCallback)(const uint8_t *line_data,
                                    uint16_t       line_num,
                                    uint16_t       width);

/**
 * @brief  Callback invoked from DMA interrupt context when a full frame ends.
 *         Equivalent to the semaphore signal but as a function pointer.
 *         Can be NULL.
 */
typedef void (*OV7670_FrameCallback)(void);

/**
 * @brief  Hardware and software configuration passed to OV7670_Init().
 *         All pointer fields must remain valid for the lifetime of the handle.
 */
typedef struct {
    /* --- SCCB (bit-bang) pins --- */
    GPIO_TypeDef *sccb_scl_port;   /**< Port for SCL (e.g. GPIOB) */
    uint16_t      sccb_scl_pin;    /**< Pin  for SCL (e.g. GPIO_PIN_6) */
    GPIO_TypeDef *sccb_sda_port;   /**< Port for SDA (e.g. GPIOB) */
    uint16_t      sccb_sda_pin;    /**< Pin  for SDA (e.g. GPIO_PIN_7) */

    /* --- Hardware control pins --- */
    GPIO_TypeDef *reset_port;      /**< RESET pin port (active low) */
    uint16_t      reset_pin;
    GPIO_TypeDef *pwdn_port;       /**< PWDN  pin port (active high = sleep) */
    uint16_t      pwdn_pin;

    /* --- Pixel data port ---
     * D0–D7 MUST be wired to bits [7:0] of a single GPIO port (PC0–PC7). */
    GPIO_TypeDef *data_port;       /**< GPIO port for D0–D7 (e.g. GPIOC) */

    /* --- TIM2 + DMA handles (initialised by CubeMX) --- */
    TIM_HandleTypeDef *htim;       /**< TIM2 handle */
    uint32_t           tim_channel;/**< TIM_CHANNEL_1 */
    DMA_HandleTypeDef *hdma;       /**< DMA1_CH5 handle linked to TIM2_CH1 */

    /* --- Image parameters --- */
    OV7670_Resolution resolution;
    OV7670_Format     format;

    /* --- Callbacks (both can be NULL) --- */
    OV7670_LineCallback  line_callback;   /**< Called after each DMA line transfer */
    OV7670_FrameCallback frame_callback;  /**< Called after the last line */

    /* --- Optional full-frame buffer ---
     * Set to a valid pointer + size to accumulate the complete frame here.
     * Set to NULL to use line_callback only (saves RAM).
     * Buffer must be at least width × height × bytes_per_pixel bytes. */
    uint8_t  *frame_buffer;
    uint32_t  frame_buffer_size;
} OV7670_Config;

/**
 * @brief  Internal driver state. Zero-initialise before calling OV7670_Init().
 *         Do not access fields directly from application code.
 */
typedef struct {
    OV7670_Config     cfg;            /**< Copy of user configuration */
    uint16_t          width;          /**< Pixels per line */
    uint16_t          height;         /**< Lines per frame */
    uint16_t          line_bytes;     /**< Bytes per line (width × bpp) */

    /* Line buffer (allocated statically inside the driver — 320 bytes max) */
    uint8_t           _line_buf[320]; /**< Internal line buffer (QQVGA RGB565 = 320 B) */

    volatile uint16_t line_count;     /**< Lines captured in current frame */
    volatile uint8_t  frame_ready;    /**< Set when a complete frame is available */
    volatile uint8_t  capturing;      /**< Non-zero while capture is active */

    SemaphoreHandle_t frame_sem;      /**< Binary semaphore — given on frame complete */
} OV7670_Handle;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief  Initialise camera hardware and configure OV7670 registers.
 *
 *         Sequence:
 *           1. Hard reset via RESET/PWDN pins.
 *           2. SCCB init.
 *           3. OV7670 soft reset and register table write.
 *           4. FreeRTOS semaphore creation.
 *           5. DMA callback registration.
 *
 * @param  hcam  Pointer to a zero-initialised OV7670_Handle.
 * @param  cfg   Configuration. Copied into hcam.
 * @return HAL_OK on success.
 *         HAL_ERROR if PID verification fails or SCCB is unresponsive.
 */
HAL_StatusTypeDef OV7670_Init(OV7670_Handle *hcam, const OV7670_Config *cfg);

/**
 * @brief  Write one register via SCCB.
 *
 * @param  hcam  Initialised camera handle.
 * @param  reg   Register address.
 * @param  val   Value to write.
 */
HAL_StatusTypeDef OV7670_WriteReg(OV7670_Handle *hcam, uint8_t reg, uint8_t val);

/**
 * @brief  Read one register via SCCB.
 *
 * @param  hcam  Initialised camera handle.
 * @param  reg   Register address.
 * @param  val   Output pointer.
 */
HAL_StatusTypeDef OV7670_ReadReg(OV7670_Handle *hcam, uint8_t reg, uint8_t *val);

/**
 * @brief  Start continuous frame capture.
 *         Arms the first DMA line transfer; subsequent lines auto-restart
 *         from the DMA complete callback.
 *
 * @param  hcam  Initialised camera handle.
 * @return HAL_OK or HAL_ERROR.
 */
HAL_StatusTypeDef OV7670_StartCapture(OV7670_Handle *hcam);

/**
 * @brief  Stop capture and disable DMA + TIM2.
 *
 * @param  hcam  Initialised camera handle.
 */
void OV7670_StopCapture(OV7670_Handle *hcam);

/**
 * @brief  Block the calling FreeRTOS task until a complete frame is ready.
 *
 * @param  hcam     Initialised camera handle.
 * @param  timeout  FreeRTOS tick timeout (use portMAX_DELAY to wait forever).
 * @return pdTRUE if a frame arrived within timeout, pdFALSE on timeout.
 */
BaseType_t OV7670_WaitFrame(OV7670_Handle *hcam, TickType_t timeout);

/**
 * @brief  Call this from your HAL_GPIO_EXTI_Callback() for the VSYNC pin.
 *         The function resets the line counter and arms the first DMA
 *         transfer for the new frame.
 *
 * @param  hcam  Initialised camera handle.
 *
 * @note   VSYNC EXTI must be configured for FALLING edge (frame start).
 */
void OV7670_VSYNC_Callback(OV7670_Handle *hcam);

/**
 * @brief  Call this from your HAL_DMA_IRQHandler() or DMA complete callback
 *         if you registered the DMA manually.
 *         Normally wired automatically by OV7670_Init() — you do not need
 *         to call this unless you handle DMA callbacks yourself.
 *
 * @param  hdma  DMA handle from the interrupt.
 */
void OV7670_DMA_CpltCallback(DMA_HandleTypeDef *hdma);

/**
 * @brief  Return the combined PID+VER word (0x7673 for genuine OV7670).
 *
 * @param  hcam  Initialised camera handle.
 * @return (PID << 8) | VER, or 0 on SCCB error.
 */
uint16_t OV7670_GetPID(OV7670_Handle *hcam);

#endif /* OV7670_H */
