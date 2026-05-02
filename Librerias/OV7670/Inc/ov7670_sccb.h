/**
 * @file    ov7670_sccb.h
 * @brief   SCCB (Serial Camera Control Bus) bit-bang driver for OV7670.
 *
 * SCCB is electrically identical to I2C but the OV7670 does not fully comply
 * with the I2C specification (no ACK on writes, specific NA phase on reads).
 * Using hardware I2C can cause bus errors, so this driver implements SCCB in
 * software using two open-drain GPIO pins with 4.7 kΩ external pull-ups.
 *
 * Timing target: ~100 kHz (5 µs half-period).
 * Timing is implemented with DWT cycle counter — call SCCB_Init() after
 * enabling the DWT unit (done automatically in SCCB_Init).
 *
 * Pin requirements in STM32CubeMX:
 *   SCL pin → GPIO Output, Open-Drain, No pull, speed Low
 *   SDA pin → GPIO Output, Open-Drain, No pull, speed Low
 *   External 4.7 kΩ pull-ups to 3.3 V on both lines are mandatory.
 */

#ifndef OV7670_SCCB_H
#define OV7670_SCCB_H

#include "stm32f1xx_hal.h"

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief  Initialize SCCB bit-bang driver.
 *         Enables the DWT cycle counter used for µs delays.
 *         Both lines are released (driven high through pull-ups).
 *
 * @param  scl_port  GPIO port of SCL pin
 * @param  scl_pin   GPIO pin mask of SCL (e.g. GPIO_PIN_6)
 * @param  sda_port  GPIO port of SDA pin
 * @param  sda_pin   GPIO pin mask of SDA (e.g. GPIO_PIN_7)
 */
void SCCB_Init(GPIO_TypeDef *scl_port, uint16_t scl_pin,
               GPIO_TypeDef *sda_port, uint16_t sda_pin);

/**
 * @brief  Write one byte to an OV7670 register (SCCB 3-phase write).
 *
 * @param  dev_addr  7-bit device address (use OV7670_SCCB_ADDR = 0x21)
 * @param  reg       Register address
 * @param  data      Value to write
 * @return HAL_OK on success, HAL_ERROR on SCCB fault
 */
HAL_StatusTypeDef SCCB_WriteReg(uint8_t dev_addr, uint8_t reg, uint8_t data);

/**
 * @brief  Read one byte from an OV7670 register (SCCB 2-cycle read).
 *         Cycle 1: write register address.
 *         Cycle 2: read data (NA phase instead of ACK).
 *
 * @param  dev_addr  7-bit device address
 * @param  reg       Register address to read
 * @param  data      Output pointer for read byte
 * @return HAL_OK on success, HAL_ERROR on SCCB fault
 */
HAL_StatusTypeDef SCCB_ReadReg(uint8_t dev_addr, uint8_t reg, uint8_t *data);

#endif /* OV7670_SCCB_H */
