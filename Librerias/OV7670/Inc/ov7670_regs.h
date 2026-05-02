/**
 * @file    ov7670_regs.h
 * @brief   OV7670 register map and bit-field definitions.
 *
 * All register addresses and named constants come from the OV7670/OV7171
 * datasheet (version 2.01). Values marked "reserved" must not be modified
 * unless explicitly stated.
 */

#ifndef OV7670_REGS_H
#define OV7670_REGS_H

/* -------------------------------------------------------------------------
 * SCCB slave address (7-bit)
 * OV7670 hardware address is fixed. SCCB write = 0x42, read = 0x43.
 * ------------------------------------------------------------------------- */
#define OV7670_SCCB_ADDR        0x21U
#define OV7670_SCCB_WRITE       (OV7670_SCCB_ADDR << 1U)
#define OV7670_SCCB_READ        ((OV7670_SCCB_ADDR << 1U) | 1U)

/* -------------------------------------------------------------------------
 * Register addresses
 * ------------------------------------------------------------------------- */
#define OV7670_REG_GAIN         0x00U
#define OV7670_REG_BLUE         0x01U
#define OV7670_REG_RED          0x02U
#define OV7670_REG_VREF         0x03U
#define OV7670_REG_COM1         0x04U
#define OV7670_REG_BAVE         0x05U
#define OV7670_REG_GbAVE        0x06U
#define OV7670_REG_AECHH        0x07U
#define OV7670_REG_RAVE         0x08U
#define OV7670_REG_COM2         0x09U
#define OV7670_REG_PID          0x0AU  /* Product ID MSB — expected 0x76 */
#define OV7670_REG_VER          0x0BU  /* Product ID LSB — expected 0x73 */
#define OV7670_REG_COM3         0x0CU
#define OV7670_REG_COM4         0x0DU
#define OV7670_REG_COM5         0x0EU
#define OV7670_REG_COM6         0x0FU
#define OV7670_REG_AECH         0x10U
#define OV7670_REG_CLKRC        0x11U
#define OV7670_REG_COM7         0x12U
#define OV7670_REG_COM8         0x13U
#define OV7670_REG_COM9         0x14U
#define OV7670_REG_COM10        0x15U
#define OV7670_REG_HSTART       0x17U
#define OV7670_REG_HSTOP        0x18U
#define OV7670_REG_VSTRT        0x19U
#define OV7670_REG_VSTOP        0x1AU
#define OV7670_REG_PSHFT        0x1BU
#define OV7670_REG_MIDH         0x1CU  /* Manufacturer ID MSB = 0x7F */
#define OV7670_REG_MIDL         0x1DU  /* Manufacturer ID LSB = 0xA2 */
#define OV7670_REG_MVFP         0x1EU
#define OV7670_REG_AEW          0x24U
#define OV7670_REG_AEB          0x25U
#define OV7670_REG_VPT          0x26U
#define OV7670_REG_EXHCH        0x2AU
#define OV7670_REG_EXHCL        0x2BU
#define OV7670_REG_ADVFL        0x2DU
#define OV7670_REG_ADVFH        0x2EU
#define OV7670_REG_YAVE         0x2FU
#define OV7670_REG_HREF         0x32U
#define OV7670_REG_TSLB         0x3AU
#define OV7670_REG_COM11        0x3BU
#define OV7670_REG_COM12        0x3CU
#define OV7670_REG_COM13        0x3DU
#define OV7670_REG_COM14        0x3EU
#define OV7670_REG_EDGE         0x3FU
#define OV7670_REG_COM15        0x40U
#define OV7670_REG_COM16        0x41U
#define OV7670_REG_COM17        0x42U
#define OV7670_REG_MTX1         0x4FU
#define OV7670_REG_MTX2         0x50U
#define OV7670_REG_MTX3         0x51U
#define OV7670_REG_MTX4         0x52U
#define OV7670_REG_MTX5         0x53U
#define OV7670_REG_MTX6         0x54U
#define OV7670_REG_BRIGHT       0x55U
#define OV7670_REG_CONTRAS      0x56U
#define OV7670_REG_CONTRAS_CTR  0x57U
#define OV7670_REG_MTXS         0x58U
#define OV7670_REG_LCC1         0x62U
#define OV7670_REG_LCC2         0x63U
#define OV7670_REG_LCC3         0x64U
#define OV7670_REG_LCC4         0x65U
#define OV7670_REG_LCC5         0x66U
#define OV7670_REG_MANU         0x67U
#define OV7670_REG_MANV         0x68U
#define OV7670_REG_GFIX         0x69U
#define OV7670_REG_DBLV         0x6BU
#define OV7670_REG_AWBCTR3      0x6CU
#define OV7670_REG_AWBCTR2      0x6DU
#define OV7670_REG_AWBCTR1      0x6EU
#define OV7670_REG_AWBCTR0      0x6FU
#define OV7670_REG_SCALING_XSC      0x70U
#define OV7670_REG_SCALING_YSC      0x71U
#define OV7670_REG_SCALING_DCWCTR   0x72U
#define OV7670_REG_SCALING_PCLK_DIV 0x73U
#define OV7670_REG_REG74            0x74U
#define OV7670_REG_SLOP         0x7AU
#define OV7670_REG_GAM1         0x7BU
#define OV7670_REG_GAM2         0x7CU
#define OV7670_REG_GAM3         0x7DU
#define OV7670_REG_GAM4         0x7EU
#define OV7670_REG_GAM5         0x7FU
#define OV7670_REG_GAM6         0x80U
#define OV7670_REG_GAM7         0x81U
#define OV7670_REG_GAM8         0x82U
#define OV7670_REG_GAM9         0x83U
#define OV7670_REG_GAM10        0x84U
#define OV7670_REG_GAM11        0x85U
#define OV7670_REG_GAM12        0x86U
#define OV7670_REG_GAM13        0x87U
#define OV7670_REG_GAM14        0x88U
#define OV7670_REG_GAM15        0x89U
#define OV7670_REG_RGB444       0x8CU
#define OV7670_REG_BD50ST       0x9DU
#define OV7670_REG_BD60ST       0x9EU
#define OV7670_REG_SCALING_PCLK_DELAY 0xA2U
#define OV7670_REG_BD50MAX      0xA5U
#define OV7670_REG_BD60MAX      0xABU
#define OV7670_REG_ABLC1        0xB1U
#define OV7670_REG_THL_ST       0xB3U
#define OV7670_REG_SATCTR       0xC9U

/* Sentinel: marks end of a register init table */
#define OV7670_REG_END          0xFFU

/* -------------------------------------------------------------------------
 * COM7 (0x12) — Output format and frame size
 * ------------------------------------------------------------------------- */
#define COM7_RESET              0x80U  /* Soft reset — all registers go to default */
#define COM7_FMT_VGA            0x00U
#define COM7_FMT_CIF            0x20U
#define COM7_FMT_QVGA           0x10U
#define COM7_FMT_QCIF           0x08U
#define COM7_RGB                0x04U  /* Enable RGB output */
#define COM7_YUV                0x00U  /* YUV output (default) */
#define COM7_BAYER              0x01U

/* -------------------------------------------------------------------------
 * COM10 (0x15) — Sync signal polarity and PCLK gating
 * ------------------------------------------------------------------------- */
#define COM10_VSYNC_NEG         0x02U  /* VSYNC active low */
#define COM10_HREF_RVSN         0x08U  /* HREF reversed */
#define COM10_PCLK_REV          0x10U  /* PCLK active low */
/* Gate PCLK so it only toggles when HREF is high.
 * This is critical for the TIM2+DMA capture strategy: DMA only gets
 * triggered by valid pixel clocks, not during blanking intervals. */
#define COM10_PCLK_GATE_HREF    0x20U
#define COM10_HREF_AS_HSYNC     0x40U

/* -------------------------------------------------------------------------
 * COM14 (0x3E) — PCLK divider for DSP scaling
 * ------------------------------------------------------------------------- */
#define COM14_DCWSEL            0x10U  /* Enable DCW and scaling PCLK */
#define COM14_PCLK_DIV1         0x00U
#define COM14_PCLK_DIV2         0x01U
#define COM14_PCLK_DIV4         0x02U
#define COM14_PCLK_DIV8         0x03U
#define COM14_PCLK_DIV16        0x04U

/* -------------------------------------------------------------------------
 * COM15 (0x40) — Output range and RGB format
 * ------------------------------------------------------------------------- */
#define COM15_R00FF             0xC0U  /* Output range [00]–[FF] */
#define COM15_R01FE             0x80U  /* Output range [01]–[FE] */
#define COM15_RGB565            0x10U  /* RGB 565 */
#define COM15_RGB555            0x30U  /* RGB 555 */

/* -------------------------------------------------------------------------
 * TSLB (0x3A) — Byte output order for YUV
 * ------------------------------------------------------------------------- */
#define TSLB_YUYV               0x00U  /* Y0 U0 Y1 V0 ... (default) */
#define TSLB_YVYU               0x04U

/* -------------------------------------------------------------------------
 * CLKRC (0x11) — Internal clock prescaler
 * Internal CLK = XCLK / (2 * (CLKRC[5:0] + 1))
 * With XCLK = 8 MHz:
 *   CLKRC = 0x01 → 2 MHz
 *   CLKRC = 0x03 → 1 MHz
 *   CLKRC = 0x07 → 500 kHz
 * ------------------------------------------------------------------------- */
#define CLKRC_DIV(n)            ((n) & 0x3FU)

/* -------------------------------------------------------------------------
 * MVFP (0x1E) — Mirror and flip
 * ------------------------------------------------------------------------- */
#define MVFP_MIRROR             0x20U
#define MVFP_FLIP               0x10U

/* -------------------------------------------------------------------------
 * Product identification
 * ------------------------------------------------------------------------- */
#define OV7670_PID_VALUE        0x76U
#define OV7670_VER_VALUE        0x73U

#endif /* OV7670_REGS_H */
