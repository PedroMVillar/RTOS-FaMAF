/**
 * @file    ov7670.c
 * @brief   OV7670 camera driver — register init, TIM2+DMA capture, FreeRTOS sync.
 */

#include "ov7670.h"
#include <string.h>

/* =========================================================================
 * Global driver instance pointer
 * Only one OV7670 is supported per MCU (F103RB has one TIM2 DMA channel).
 * ========================================================================= */
static OV7670_Handle *s_hcam = NULL;

/* =========================================================================
 * Register init tables
 * Format: { reg, value }. Table ends with { OV7670_REG_END, 0 }.
 *
 * Clock budget (with XCLK = 8 MHz from MCO/HSE):
 *   CLKRC = 0x01 → internal CLK = 8 / (2×2) = 2 MHz
 *   COM14 PCLK_DIV2 + DCWSEL → PCLK ≈ 1 MHz after DSP scaling
 *   At 1 MHz PCLK, F103 at 72 MHz has 72 cycles per pixel — comfortable for DMA.
 *   Approximate frame rate QQVGA: ~6 fps.
 * ========================================================================= */

/* --- Common base settings shared by all modes --- */
static const uint8_t s_common_regs[][2] = {
    /* Soft reset — must be first; wait 30 ms after this in code */
    { OV7670_REG_COM7,  COM7_RESET },

    /* Clock: internal = XCLK / (2*(1+1)) = 2 MHz */
    { OV7670_REG_CLKRC, CLKRC_DIV(1) },

    /* Automatic gain/exposure control enabled */
    { OV7670_REG_COM8,  0xE5 },

    /* AEC stable region */
    { OV7670_REG_AEW,   0x75 },
    { OV7670_REG_AEB,   0x63 },
    { OV7670_REG_VPT,   0xA5 },

    /* No mirror, no flip */
    { OV7670_REG_MVFP,  0x00 },

    /* AWB */
    { OV7670_REG_AWBCTR3, 0x0A },
    { OV7670_REG_AWBCTR2, 0x55 },
    { OV7670_REG_AWBCTR1, 0x11 },
    { OV7670_REG_AWBCTR0, 0x9E },

    /* Gamma curve (gentle S-curve, good for visual output) */
    { OV7670_REG_SLOP, 0x20 },
    { OV7670_REG_GAM1, 0x1C }, { OV7670_REG_GAM2, 0x28 },
    { OV7670_REG_GAM3, 0x3C }, { OV7670_REG_GAM4, 0x55 },
    { OV7670_REG_GAM5, 0x68 }, { OV7670_REG_GAM6, 0x76 },
    { OV7670_REG_GAM7, 0x80 }, { OV7670_REG_GAM8, 0x88 },
    { OV7670_REG_GAM9, 0x8F }, { OV7670_REG_GAM10, 0x96 },
    { OV7670_REG_GAM11, 0xA3 }, { OV7670_REG_GAM12, 0xAF },
    { OV7670_REG_GAM13, 0xC4 }, { OV7670_REG_GAM14, 0xD7 },
    { OV7670_REG_GAM15, 0xE8 },

    /* Colour matrix for natural colours */
    { OV7670_REG_MTX1, 0x80 }, { OV7670_REG_MTX2, 0x80 },
    { OV7670_REG_MTX3, 0x00 }, { OV7670_REG_MTX4, 0x22 },
    { OV7670_REG_MTX5, 0x5E }, { OV7670_REG_MTX6, 0x80 },
    { OV7670_REG_MTXS, 0x9E },
    { OV7670_REG_COM13, 0xC0 },

    /* ABLC (auto black-level calibration) */
    { OV7670_REG_ABLC1, 0x08 },
    { OV7670_REG_THL_ST, 0x80 },

    /* Banding filter for 50 Hz fluorescent light */
    { OV7670_REG_COM11, 0x0A },
    { OV7670_REG_BD50ST, 0x99 },
    { OV7670_REG_BD60ST, 0x7F },

    { OV7670_REG_END, 0x00 }
};

/* --- QQVGA (160×120) RGB565 ---
 * Strategy: configure sensor for QVGA (320×240) then use DSP 2× downscale.
 * This gives better image quality than windowing to native QQVGA. */
static const uint8_t s_qqvga_rgb565_regs[][2] = {
    /* Output: QVGA size, RGB mode */
    { OV7670_REG_COM7,  COM7_FMT_QVGA | COM7_RGB },

    /* PCLK gates on HREF=0 — DMA only fires for valid pixels */
    { OV7670_REG_COM10, COM10_PCLK_GATE_HREF },

    /* RGB565 output, full range [0x00–0xFF] */
    { OV7670_REG_COM15, COM15_R00FF | COM15_RGB565 },

    /* DSP 2× downscale: QVGA→QQVGA */
    { OV7670_REG_COM3,  0x04 },           /* DCW enable */
    { OV7670_REG_COM14, COM14_DCWSEL | COM14_PCLK_DIV2 },
    { OV7670_REG_SCALING_XSC,      0x3A },
    { OV7670_REG_SCALING_YSC,      0x35 },
    { OV7670_REG_SCALING_DCWCTR,   0x11 }, /* /2 horizontal, /2 vertical */
    { OV7670_REG_SCALING_PCLK_DIV, 0xF1 }, /* DSP clock /2 */
    { OV7670_REG_SCALING_PCLK_DELAY, 0x02 },

    /* Window registers for QVGA timing base (standard values) */
    { OV7670_REG_HSTART, 0x16 },
    { OV7670_REG_HSTOP,  0x04 },
    { OV7670_REG_HREF,   0x24 },
    { OV7670_REG_VSTRT,  0x02 },
    { OV7670_REG_VSTOP,  0x7A },
    { OV7670_REG_VREF,   0x0A },

    /* TSLB: YUYV byte order (ignored in RGB mode, but set for safety) */
    { OV7670_REG_TSLB,   TSLB_YUYV },

    { OV7670_REG_END, 0x00 }
};

/* --- QQVGA (160×120) YUV422 (YUYV) --- */
static const uint8_t s_qqvga_yuv422_regs[][2] = {
    { OV7670_REG_COM7,  COM7_FMT_QVGA | COM7_YUV },
    { OV7670_REG_COM10, COM10_PCLK_GATE_HREF },
    { OV7670_REG_COM15, COM15_R00FF },
    { OV7670_REG_COM3,  0x04 },
    { OV7670_REG_COM14, COM14_DCWSEL | COM14_PCLK_DIV2 },
    { OV7670_REG_SCALING_XSC,      0x3A },
    { OV7670_REG_SCALING_YSC,      0x35 },
    { OV7670_REG_SCALING_DCWCTR,   0x11 },
    { OV7670_REG_SCALING_PCLK_DIV, 0xF1 },
    { OV7670_REG_SCALING_PCLK_DELAY, 0x02 },
    { OV7670_REG_HSTART, 0x16 },
    { OV7670_REG_HSTOP,  0x04 },
    { OV7670_REG_HREF,   0x24 },
    { OV7670_REG_VSTRT,  0x02 },
    { OV7670_REG_VSTOP,  0x7A },
    { OV7670_REG_VREF,   0x0A },
    { OV7670_REG_TSLB,   TSLB_YUYV },
    { OV7670_REG_END, 0x00 }
};

/* --- QVGA (320×240) RGB565 (line_callback mode recommended — no full-frame buffer) --- */
static const uint8_t s_qvga_rgb565_regs[][2] = {
    { OV7670_REG_COM7,  COM7_FMT_QVGA | COM7_RGB },
    { OV7670_REG_COM10, COM10_PCLK_GATE_HREF },
    { OV7670_REG_COM15, COM15_R00FF | COM15_RGB565 },
    /* No downscale for QVGA */
    { OV7670_REG_COM3,  0x00 },
    { OV7670_REG_COM14, 0x00 },
    { OV7670_REG_SCALING_XSC,      0x3A },
    { OV7670_REG_SCALING_YSC,      0x35 },
    { OV7670_REG_SCALING_DCWCTR,   0x00 },
    { OV7670_REG_SCALING_PCLK_DIV, 0xF0 },
    { OV7670_REG_SCALING_PCLK_DELAY, 0x02 },
    { OV7670_REG_HSTART, 0x16 },
    { OV7670_REG_HSTOP,  0x04 },
    { OV7670_REG_HREF,   0x24 },
    { OV7670_REG_VSTRT,  0x02 },
    { OV7670_REG_VSTOP,  0x7A },
    { OV7670_REG_VREF,   0x0A },
    { OV7670_REG_TSLB,   TSLB_YUYV },
    { OV7670_REG_END, 0x00 }
};

/* =========================================================================
 * Private helpers
 * ========================================================================= */

static HAL_StatusTypeDef _write_table(OV7670_Handle *hcam,
                                      const uint8_t  table[][2])
{
    HAL_StatusTypeDef ret = HAL_OK;
    for (uint16_t i = 0; table[i][0] != OV7670_REG_END; i++) {
        ret = SCCB_WriteReg(OV7670_SCCB_ADDR, table[i][0], table[i][1]);
        if (ret != HAL_OK) {
            return ret;
        }
        /* Small gap between consecutive SCCB writes per OV7670 app note */
        HAL_Delay(1);
    }
    return ret;
}

/* Arm one DMA line transfer from data_port→IDR to internal line buffer. */
static HAL_StatusTypeDef _arm_dma(OV7670_Handle *hcam)
{
    /* Stop any previous DMA state */
    __HAL_DMA_DISABLE(hcam->cfg.hdma);

    HAL_StatusTypeDef ret = HAL_DMA_Start_IT(
        hcam->cfg.hdma,
        (uint32_t)hcam->cfg.data_port,  /* source: GPIO IDR (LSB = D0–D7) */
        (uint32_t)hcam->_line_buf,       /* destination: internal line buffer */
        hcam->line_bytes
    );

    if (ret != HAL_OK) {
        return ret;
    }

    /* Enable TIM2 CH1 DMA request so each input capture triggers a transfer */
    __HAL_TIM_ENABLE_DMA(hcam->cfg.htim, TIM_DMA_CC1);
    __HAL_TIM_ENABLE(hcam->cfg.htim);

    return HAL_OK;
}

/* =========================================================================
 * DMA complete callback (registered with HAL in OV7670_Init)
 * Runs in interrupt context — keep it short.
 * ========================================================================= */
static void _dma_cplt_callback(DMA_HandleTypeDef *hdma)
{
    if (s_hcam == NULL || hdma != s_hcam->cfg.hdma) {
        return;
    }

    /* Disable TIM DMA request until next line is armed */
    __HAL_TIM_DISABLE_DMA(s_hcam->cfg.htim, TIM_DMA_CC1);

    uint16_t line = s_hcam->line_count;
    s_hcam->line_count++;

    /* Copy to frame_buffer if the user provided one */
    if (s_hcam->cfg.frame_buffer != NULL) {
        uint32_t offset = (uint32_t)line * s_hcam->line_bytes;
        if ((offset + s_hcam->line_bytes) <= s_hcam->cfg.frame_buffer_size) {
            memcpy(s_hcam->cfg.frame_buffer + offset,
                   s_hcam->_line_buf,
                   s_hcam->line_bytes);
        }
    }

    /* Notify user for per-line processing */
    if (s_hcam->cfg.line_callback != NULL) {
        s_hcam->cfg.line_callback(s_hcam->_line_buf, line, s_hcam->width);
    }

    if (s_hcam->line_count < s_hcam->height) {
        /* More lines to go — re-arm DMA immediately */
        _arm_dma(s_hcam);
    } else {
        /* Frame complete */
        s_hcam->frame_ready = 1U;
        s_hcam->capturing   = 0U;

        if (s_hcam->cfg.frame_callback != NULL) {
            s_hcam->cfg.frame_callback();
        }

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(s_hcam->frame_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

HAL_StatusTypeDef OV7670_Init(OV7670_Handle *hcam, const OV7670_Config *cfg)
{
    if (hcam == NULL || cfg == NULL) {
        return HAL_ERROR;
    }

    memset(hcam, 0, sizeof(OV7670_Handle));
    hcam->cfg = *cfg;
    s_hcam    = hcam;

    /* Resolve image dimensions */
    switch (cfg->resolution) {
        case OV7670_RES_QQVGA:
            hcam->width  = 160U;
            hcam->height = 120U;
            break;
        case OV7670_RES_QVGA:
            hcam->width  = 320U;
            hcam->height = 240U;
            break;
        default:
            return HAL_ERROR;
    }
    hcam->line_bytes = hcam->width * 2U; /* RGB565 and YUV422 are both 2 B/px */

    /* Verify line buffer fits in the internal buffer */
    if (hcam->line_bytes > sizeof(hcam->_line_buf)) {
        return HAL_ERROR;
    }

    /* ---- Hardware reset sequence ---------------------------------------- */
    /* Power down */
    HAL_GPIO_WritePin(cfg->pwdn_port, cfg->pwdn_pin, GPIO_PIN_SET);
    HAL_Delay(10);
    /* Assert RESET */
    HAL_GPIO_WritePin(cfg->reset_port, cfg->reset_pin, GPIO_PIN_RESET);
    HAL_Delay(10);
    /* Power on */
    HAL_GPIO_WritePin(cfg->pwdn_port, cfg->pwdn_pin, GPIO_PIN_RESET);
    HAL_Delay(10);
    /* Release RESET */
    HAL_GPIO_WritePin(cfg->reset_port, cfg->reset_pin, GPIO_PIN_SET);
    HAL_Delay(30); /* OV7670 needs ≥ 20 ms after reset before first SCCB write */

    /* ---- SCCB init ------------------------------------------------------- */
    SCCB_Init(cfg->sccb_scl_port, cfg->sccb_scl_pin,
              cfg->sccb_sda_port, cfg->sccb_sda_pin);

    /* ---- Verify product ID ----------------------------------------------- */
    uint16_t pid = OV7670_GetPID(hcam);
    if (pid != (((uint16_t)OV7670_PID_VALUE << 8U) | OV7670_VER_VALUE)) {
        /* If your module returns 0x0000 or 0xFFFF, check SCCB wiring and
         * pull-ups. A PID mismatch (e.g. 0x7671) is a different OV76xx
         * variant — the register table below will still mostly work. */
        return HAL_ERROR;
    }

    /* ---- Write common + format register tables --------------------------- */
    /* Soft reset is included in s_common_regs[0] */
    HAL_StatusTypeDef ret = _write_table(hcam, s_common_regs);
    if (ret != HAL_OK) {
        return ret;
    }
    HAL_Delay(30); /* Wait for sensor to stabilise after soft reset */

    /* Write format/resolution-specific table */
    const uint8_t (*fmt_table)[2] = NULL;
    if (cfg->resolution == OV7670_RES_QQVGA && cfg->format == OV7670_FMT_RGB565) {
        fmt_table = s_qqvga_rgb565_regs;
    } else if (cfg->resolution == OV7670_RES_QQVGA && cfg->format == OV7670_FMT_YUV422) {
        fmt_table = s_qqvga_yuv422_regs;
    } else if (cfg->resolution == OV7670_RES_QVGA && cfg->format == OV7670_FMT_RGB565) {
        fmt_table = s_qvga_rgb565_regs;
    } else {
        return HAL_ERROR;
    }

    ret = _write_table(hcam, fmt_table);
    if (ret != HAL_OK) {
        return ret;
    }

    /* ---- FreeRTOS binary semaphore --------------------------------------- */
    hcam->frame_sem = xSemaphoreCreateBinary();
    if (hcam->frame_sem == NULL) {
        return HAL_ERROR;
    }

    /* ---- Register DMA complete callback with HAL ------------------------- */
    HAL_DMA_RegisterCallback(cfg->hdma,
                             HAL_DMA_XFER_CPLT_CB_ID,
                             _dma_cplt_callback);

    return HAL_OK;
}

HAL_StatusTypeDef OV7670_WriteReg(OV7670_Handle *hcam, uint8_t reg, uint8_t val)
{
    (void)hcam;
    return SCCB_WriteReg(OV7670_SCCB_ADDR, reg, val);
}

HAL_StatusTypeDef OV7670_ReadReg(OV7670_Handle *hcam, uint8_t reg, uint8_t *val)
{
    (void)hcam;
    return SCCB_ReadReg(OV7670_SCCB_ADDR, reg, val);
}

uint16_t OV7670_GetPID(OV7670_Handle *hcam)
{
    uint8_t pid = 0, ver = 0;
    if (OV7670_ReadReg(hcam, OV7670_REG_PID, &pid) != HAL_OK) {
        return 0U;
    }
    if (OV7670_ReadReg(hcam, OV7670_REG_VER, &ver) != HAL_OK) {
        return 0U;
    }
    return (uint16_t)((uint16_t)pid << 8U) | ver;
}

HAL_StatusTypeDef OV7670_StartCapture(OV7670_Handle *hcam)
{
    if (hcam == NULL || hcam->frame_sem == NULL) {
        return HAL_ERROR;
    }
    hcam->capturing   = 1U;
    hcam->frame_ready = 0U;
    hcam->line_count  = 0U;

    /* DMA is armed in OV7670_VSYNC_Callback when VSYNC fires.
     * Here we just enable the TIM so it watches for PCLK edges. */
    __HAL_TIM_ENABLE(hcam->cfg.htim);

    return HAL_OK;
}

void OV7670_StopCapture(OV7670_Handle *hcam)
{
    if (hcam == NULL) {
        return;
    }
    hcam->capturing = 0U;
    __HAL_TIM_DISABLE_DMA(hcam->cfg.htim, TIM_DMA_CC1);
    __HAL_TIM_DISABLE(hcam->cfg.htim);
    HAL_DMA_Abort(hcam->cfg.hdma);
}

BaseType_t OV7670_WaitFrame(OV7670_Handle *hcam, TickType_t timeout)
{
    if (hcam == NULL || hcam->frame_sem == NULL) {
        return pdFALSE;
    }
    return xSemaphoreTake(hcam->frame_sem, timeout);
}

void OV7670_VSYNC_Callback(OV7670_Handle *hcam)
{
    if (hcam == NULL || !hcam->capturing) {
        return;
    }

    /* Reset state for new frame */
    hcam->line_count  = 0U;
    hcam->frame_ready = 0U;

    /* Arm first DMA line transfer — subsequent lines arm from _dma_cplt_callback */
    _arm_dma(hcam);
}

/* Exposed so the application can call it from a manually-registered DMA IRQ.
 * Normally the HAL callback chain calls _dma_cplt_callback directly. */
void OV7670_DMA_CpltCallback(DMA_HandleTypeDef *hdma)
{
    _dma_cplt_callback(hdma);
}
