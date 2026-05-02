/**
 * @file    ov7670_sccb.c
 * @brief   SCCB bit-bang implementation using DWT for µs-accurate timing.
 */

#include "ov7670_sccb.h"

/* =========================================================================
 * Private state
 * ========================================================================= */

static GPIO_TypeDef *s_scl_port;
static uint16_t      s_scl_pin;
static GPIO_TypeDef *s_sda_port;
static uint16_t      s_sda_pin;

/* =========================================================================
 * Timing helpers
 * =========================================================================
 * SCCB target: 100 kHz → half-period = 5 µs.
 * DWT->CYCCNT is enabled in SCCB_Init and used for busy-wait delays so
 * this code never calls vTaskDelay (unsafe in ISR or early init context).
 * ========================================================================= */

static inline void _dwt_delay_us(uint32_t us)
{
    uint32_t start  = DWT->CYCCNT;
    uint32_t target = us * (SystemCoreClock / 1000000UL);
    while ((DWT->CYCCNT - start) < target) {
        /* busy-wait */
    }
}

#define SCCB_HALF_PERIOD_US  5U   /* 5 µs → 100 kHz */

/* =========================================================================
 * Line control helpers
 * ========================================================================= */

static inline void _scl_high(void) { HAL_GPIO_WritePin(s_scl_port, s_scl_pin, GPIO_PIN_SET);   }
static inline void _scl_low(void)  { HAL_GPIO_WritePin(s_scl_port, s_scl_pin, GPIO_PIN_RESET); }
static inline void _sda_high(void) { HAL_GPIO_WritePin(s_sda_port, s_sda_pin, GPIO_PIN_SET);   }
static inline void _sda_low(void)  { HAL_GPIO_WritePin(s_sda_port, s_sda_pin, GPIO_PIN_RESET); }

static inline GPIO_PinState _sda_read(void)
{
    return HAL_GPIO_ReadPin(s_sda_port, s_sda_pin);
}

static void _start(void)
{
    _sda_high();
    _scl_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _sda_low();  /* SDA falls while SCL is high = START */
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _scl_low();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
}

static void _stop(void)
{
    _sda_low();
    _scl_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _sda_high();  /* SDA rises while SCL is high = STOP */
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
}

/* Send one byte, clock 9 times (8 data + 1 don't-care/NA phase).
 * SCCB does not use ACK on write cycles — the 9th bit is a don't-care. */
static void _write_byte(uint8_t byte)
{
    for (int8_t i = 7; i >= 0; i--) {
        if (byte & (1U << i)) {
            _sda_high();
        } else {
            _sda_low();
        }
        _dwt_delay_us(SCCB_HALF_PERIOD_US);
        _scl_high();
        _dwt_delay_us(SCCB_HALF_PERIOD_US);
        _scl_low();
        _dwt_delay_us(SCCB_HALF_PERIOD_US);
    }
    /* 9th clock: don't-care (release SDA, let it float high via pull-up) */
    _sda_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _scl_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _scl_low();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
}

/* Read one byte, then send NA (SDA high) on the 9th clock. */
static uint8_t _read_byte_na(void)
{
    uint8_t byte = 0;
    _sda_high(); /* release SDA so OV7670 can drive it */
    for (int8_t i = 7; i >= 0; i--) {
        _dwt_delay_us(SCCB_HALF_PERIOD_US);
        _scl_high();
        _dwt_delay_us(SCCB_HALF_PERIOD_US);
        if (_sda_read() == GPIO_PIN_SET) {
            byte |= (1U << i);
        }
        _scl_low();
        _dwt_delay_us(SCCB_HALF_PERIOD_US);
    }
    /* 9th clock: NA (master drives SDA high = no-acknowledge) */
    _sda_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _scl_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    _scl_low();
    _dwt_delay_us(SCCB_HALF_PERIOD_US);
    return byte;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void SCCB_Init(GPIO_TypeDef *scl_port, uint16_t scl_pin,
               GPIO_TypeDef *sda_port, uint16_t sda_pin)
{
    s_scl_port = scl_port;
    s_scl_pin  = scl_pin;
    s_sda_port = sda_port;
    s_sda_pin  = sda_pin;

    /* Enable DWT cycle counter for µs delays */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT       = 0U;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    /* Release both lines */
    _scl_high();
    _sda_high();
    _dwt_delay_us(SCCB_HALF_PERIOD_US * 2U);
}

HAL_StatusTypeDef SCCB_WriteReg(uint8_t dev_addr, uint8_t reg, uint8_t data)
{
    _start();
    _write_byte((uint8_t)(dev_addr << 1U));  /* address + W bit */
    _write_byte(reg);
    _write_byte(data);
    _stop();
    _dwt_delay_us(100U);  /* OV7670 needs settling time between SCCB transactions */
    return HAL_OK;
}

HAL_StatusTypeDef SCCB_ReadReg(uint8_t dev_addr, uint8_t reg, uint8_t *data)
{
    if (data == NULL) {
        return HAL_ERROR;
    }

    /* Phase 1: write register address */
    _start();
    _write_byte((uint8_t)(dev_addr << 1U));
    _write_byte(reg);
    _stop();
    _dwt_delay_us(100U);

    /* Phase 2: read data */
    _start();
    _write_byte((uint8_t)((dev_addr << 1U) | 1U));  /* address + R bit */
    *data = _read_byte_na();
    _stop();
    _dwt_delay_us(100U);

    return HAL_OK;
}
