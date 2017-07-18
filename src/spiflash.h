/*
The MIT License (MIT)

Copyright (c) 2015 Peter Andersson (pelleplutt1976<at>gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/**
 * spiflash.h
 *
 * @author: petera
 */

#ifndef SPIFLASH_H_
#define SPIFLASH_H_

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SPIFLASH_ERR_BASE
#define _SPIFLASH_ERR_BASE            (-24000)
#endif

#define SPIFLASH_OK                   (0)
#define SPIFLASH_ERR_INTERNAL         (_SPIFLASH_ERR_BASE - 1)
#define SPIFLASH_ERR_BAD_STATE        (_SPIFLASH_ERR_BASE - 2)
#define SPIFLASH_ERR_HW_BUSY          (_SPIFLASH_ERR_BASE - 3)
#define SPIFLASH_ERR_BUSY             (_SPIFLASH_ERR_BASE - 4)
#define SPIFLASH_ERR_ERASE_UNALIGNED  (_SPIFLASH_ERR_BASE - 5)
#define SPIFLASH_ERR_BAD_CONFIG       (_SPIFLASH_ERR_BASE - 6)

#ifndef SPIF_DBG
#define SPIF_DBG(...) //printf("SPIFL:" __VA_ARGS__)
#endif

/**
 * Set if standard spi flash commands.
 */
#define SPIFLASH_CMD_TBL_STANDARD \
  (spiflash_cmd_tbl_t) { \
    .write_disable = 0x04, \
    .write_enable = 0x06, \
    .page_program = 0x02, \
    .read_data = 0x03, \
    .read_data_fast = 0x0b, \
    .write_sr = 0x01, \
    .read_sr = 0x05, \
    .block_erase_4 = 0x20, \
    .block_erase_8 = 0x00, \
    .block_erase_16 = 0x00, \
    .block_erase_32 = 0x52, \
    .block_erase_64 = 0xd8, \
    .chip_erase = 0xc7, \
    .device_id = 0x90, \
    .jedec_id = 0x9f, \
    .sr_busy_bit = 0x01, \
  }

#define SPIFLASH_SYNCHRONOUS          (0)
#define SPIFLASH_ASYNCHRONOUS         (1)
#define SPIFLASH_ENDIANNESS_LITTLE    (0)
#define SPIFLASH_ENDIANNESS_BIG       (1)

/**
 * Defines the hardware commands for given spi flash. These numbers are found
 * in the data sheet. Set to zero (0x00) if some command is not supported. A
 * standard set is found in macro SPIFLASH_CMD_TBL_STANDARD.
 */
typedef struct spiflash_cmd_tbl_s {
  uint8_t write_disable;
  uint8_t write_enable;

  uint8_t page_program;
  uint8_t read_data;
  uint8_t read_data_fast;
  
  uint8_t write_sr;
  uint8_t read_sr;
  
  uint8_t block_erase_4;
  uint8_t block_erase_8;
  uint8_t block_erase_16;
  uint8_t block_erase_32;
  uint8_t block_erase_64;
  uint8_t chip_erase;

  uint8_t device_id;
  uint8_t jedec_id;
  
  // indicate which bit in the SR which is the busy flag bit
  uint8_t sr_busy_bit;
} spiflash_cmd_tbl_t;

struct spiflash_s;

/**
 * Defines the hardware abstraction layer for the spi flash driver.
 */
typedef struct spiflash_hal_s {
  /**
   * Carry out a spi transaction.
   * First, if tx_len > 0, tx_data is to be transmitted from tx_data.
   * Then, if rx_len > 0, rx_data is to be received into rx_data.
   * This function is called when the spi flash driver needs to communicate on
   * the SPI bus. When the transaction is finished, spiflash_async_cb is to be
   * called in asynchronous mode. 
   * In synchronous mode, this must block.
   * 
   * @param spi      pointer to the spi flash driver struct.
   * @param tx_data  data to be transmitted, ignored if tx_len == 0.
   * @param tx_len   length of data to be transmitted.
   * @param rx_data  data to be received, ignored if rx_len == 0.
   * @param rx_len   length of data to be received.
   * @return 0 if ok, anything else is considered an error.
   */
  int (*_spiflash_spi_txrx)(struct spiflash_s *spi, const uint8_t *tx_data,
      uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len);

  /**
   * Assert/deassert chip select. If parameter cs is nonzero, CS pin should be
   * asserted (normally means the pin should be at gnd level, low). If
   * parameter cs is zero, CS pin should be deasserted (normally means the pin
   * should be at Vddio level, high).
   *
   * @param spi  pointer to the spi flash driver struct.
   * @param cs   !0 to assert CS, 0 to deassert CS.
   */
  void (*_spiflash_spi_cs)(struct spiflash_s *spi, uint8_t cs);
  
  /**
   * Wait given number of milliseconds.
   * When the timeout is reached, spiflash_async_trigger is to be called in
   * asynchronous mode.
   * In synchronous mode, this must block given number of milliseconds.
   * 
   * @param spi  pointer to the spi flash driver struct.
   */
  void (*_spiflash_wait)(struct spiflash_s *spi, uint32_t ms);
} spiflash_hal_t;

/**
 * Spi flash configuration. These values are found in the data sheet.
 * If busy pin is wired to your processor somehow, set the *_ms values to zero.
 * Prior to waiting for busy pin, _spiflash_wait_async will be called with 0
 * ms. When busy pin releases, call spiflash_async_trigger.
 */
typedef struct spiflash_config_s {
  // size of flash in bytes
  uint32_t sz;
  // page size of flash in bytes
  uint32_t page_sz;
  // address size in bytes
  uint8_t addr_sz;
  // address extra dummy bytes, added to read/fast read/write/erase block
  // commands
  uint8_t addr_dummy_sz;
  // address endinness (big endian [0x01234567 = 0x01 0x23 0x45 0x67], 
  //                    little endian [0x01234567 = 0x67 0x45 0x23 0x01])
  // normally big endian
  uint8_t addr_endian;
  
  // typical write sr time in ms
  uint32_t sr_write_ms;
  // typical page program time in ms
  uint32_t page_program_ms;
  // typical 4k block erase time in ms
  uint32_t block_erase_4_ms;
  // typical 8k block erase time in ms
  uint32_t block_erase_8_ms;
  // typical 16k block erase time in ms
  uint32_t block_erase_16_ms;
  // typical 32k block erase time in ms
  uint32_t block_erase_32_ms;
  // typical 64k block erase time in ms
  uint32_t block_erase_64_ms;
  // typical chip erase time in ms
  uint32_t chip_erase_ms;
} spiflash_config_t;

/**
 * Spi flash device operation enum.
 */
typedef enum {
  SPIFLASH_OP_IDLE = 0,
  SPIFLASH_OP_ERASE_BLOCK_sWREN,
  SPIFLASH_OP_ERASE_BLOCK_sERAS,
  SPIFLASH_OP_ERASE_CHIP_sWREN,
  SPIFLASH_OP_ERASE_CHIP_sERAS,
  SPIFLASH_OP_WRITE_sWREN,
  SPIFLASH_OP_WRITE_sADDR,
  SPIFLASH_OP_WRITE_sDATA,
  SPIFLASH_OP_WRITE_SR_sWREN,
  SPIFLASH_OP_WRITE_SR_sDATA,
  SPIFLASH_OP_WRITE_REG_sWREN,
  SPIFLASH_OP_WRITE_REG_sDATAWAIT,
  SPIFLASH_OP_WRITE_REG_DATA,
  SPIFLASH_OP_READ,
  SPIFLASH_OP_FAST_READ,
  SPIFLASH_OP_READ_SR,
  SPIFLASH_OP_READ_SR_BUSY,
  SPIFLASH_OP_READ_JEDEC,
  SPIFLASH_OP_READ_PRODUCT,
  SPIFLASH_OP_READ_REG,
} spiflash_op_t;

/**
 * In asynchronous mode, this is called when an operation have finished.
 *
 * @param spi        the spiflash struct.
 * @param operation  on an error this represents the operation that failed.
 * @param err_code   the error code on error or SPIFLASH_OK.
 */
typedef void (*spiflash_cb_async_t)(struct spiflash_s *spi,
    spiflash_op_t operation, int err_code);

/**
 * The spi flash driver struct.
 */
typedef struct spiflash_s {
  // physical spi flash config
  const spiflash_config_t *cfg;
  // command table
  const spiflash_cmd_tbl_t *cmd_tbl;
  // HAL config
  const spiflash_hal_t *hal;
  // Asynchronous callback
  spiflash_cb_async_t async_cb;

  // user data for identification
  void *user_data;
  
  // internals
  uint8_t async;
  volatile spiflash_op_t op;
  uint32_t wait_period_ms;
  uint32_t addr;
  union {
    uint32_t wr_len;
    uint32_t rd_len;
    uint32_t erase_len;
  };
  union {
    const uint8_t *wr_buf;
    uint8_t *rd_buf;
    uint8_t *sr_dst;
    uint8_t *reg_dst;
    uint32_t *id_dst;
  };
  uint8_t could_be_busy;
  uint8_t busy_pre_check;
  uint8_t busy_check_wait;
  union {
    uint8_t reg_nbr;
    uint8_t sr_data;
    uint8_t tx_internal_buf[16];
  };
} spiflash_t;

/**
 * Initiates the spi flash device struct.
 *
 * @param spi        pointer to the spi flash driver struct.
 * @param cfg        pointer to the spi flash driver configuration struct.
 * @param cmd        pointer to the spi flash driver command struct.
 * @param hal        pointer to the spi flash driver HAL struct.
 * @param async      if the driver will handle operations asynchronously.
 * @param async_cb   pointer to the spi flash driver asynchronous callback
 *                   function. Ignored in synchronous state.
 * @param user_data  some user data.
 */
void SPIFLASH_init(spiflash_t *spi, 
                   const spiflash_config_t *cfg,
                   const spiflash_cmd_tbl_t *cmd,
                   const spiflash_hal_t *hal,
                   spiflash_cb_async_t async_cb,
                   uint8_t async,
                   void *user_data);

/**
 * Writes data to the spi flash.
 *
 * @param spi   the spi flash struct.
 * @param addr  the address of the spi flash to write to.
 * @param len   number of bytes to write.
 * @param buf   the data to write.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_write(spiflash_t *spi, uint32_t addr, uint32_t len, const uint8_t *buf);

/**
 * Erases data in the spi flash. The erase range must be aligned to the
 * smallest erase size a SPIFLASH_ERR_ERASE_UNALIGNED will be returned.
 *
 * @param spi   pointer to the spi flash driver struct.
 * @param addr  the address of the spi flash to write to.
 * @param len   number of bytes to write.
 * @param buf   the data to write.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_erase(spiflash_t *spi, uint32_t addr, uint32_t len);

/**
 * Erases the entire spi flash chip.
 *
 * @param spi  pointer to the spi flash driver struct.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_chip_erase(spiflash_t *spi);

/**
 * Writes to the status register.
 *
 * @param spi  pointer to the spi flash driver struct.
 * @param sr   the data to write to sr.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_write_sr(spiflash_t *spi, uint8_t sr);

/**
 * Reads from the spi flash.
 *
 * @param spi   pointer to the spi flash driver struct.
 * @param addr  the address to read from.
 * @param len   number of bytes to read.
 * @param buf   where to put read data.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_read(spiflash_t *spi, uint32_t addr, uint32_t len, uint8_t *buf);

/**
 * Reads from the spi flash, fast mode. Will automatically add one extra
 * dummy byte to address (apart from cfg.addr_dummy_sz). If not supported
 * (0 in cmd_tbl), a normal read will take place.
 *
 * @param spi   pointer to the spi flash driver struct.
 * @param addr  the address to read from.
 * @param len   number of bytes to read.
 * @param buf   where to put read data.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_fast_read(spiflash_t *spi, uint32_t addr, uint32_t len, 
                       uint8_t *buf);

/**
 * Reads the status register.
 *
 * @param spi  pointer to the spi flash driver struct.
 * @param sr   where to put the data of the status register.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_read_sr(spiflash_t *spi, uint8_t *sr);

/**
 * Reads the status register and parses the busy bit according to
 * cmd_tbl.sr_busy_bit.
 *
 * @param spi   pointer to the spi flash driver struct.
 * @param busy  will be set to true if busy, false otherwise.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_read_sr_busy(spiflash_t *spi, uint8_t *busy);

/**
 * Reads the jedec id of the device, 3 bytes.
 *
 * @param spi       pointer to the spi flash driver struct.
 * @param jedec_id  where to store the jedec id.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_read_jedec_id(spiflash_t *spi, uint32_t *jedec_id);

/**
 * Reads some hardware specific register.
 *
 * @param spi      pointer to the spi flash driver struct.
 * @param reg      register number
 * @param data     where to store the register contents.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_read_reg(spiflash_t *spi, uint8_t reg, uint8_t *data);

/**
 * Writes some hardware specific register.
 *
 * @param spi       pointer to the spi flash driver struct.
 * @param reg       register number
 * @param data      what to set the register to.
 * @param write_en  if write enable must be issued before writing to register.
 * @param wait_ms   if write_en is enabled, this states the typical time in
 *                  milliseconds to write to the register.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_write_reg(spiflash_t *spi, uint8_t reg, uint8_t data,
    uint8_t write_en, uint32_t wait_ms);

/**
 * Reads the product id of the device, 3 bytes.
 *
 * @param spi      pointer to the spi flash driver struct.
 * @param prod_id  where to store the product id.
 * @return error code or SPIFLASH_OK
 */
int SPIFLASH_read_product_id(spiflash_t *spi, uint32_t *prod_id);


/**
 * Returns if the driver is busy or not. Will not do any spi communication.
 *
 * @param spi  pointer to the spi flash driver struct.
 * @return true if busy, false if not.
 */
int SPIFLASH_is_busy(spiflash_t *spi);

/**
 * Call this when either hal._spiflash_spi_txrx or hal._spiflash_wait
 * have finished in asynchronous mode. Do not call in synchronous mode.
 * Even if the asynchronous operation failed, this must be called in order
 * to clean up spi driver state.
 *
 * @param spi       pointer to the spi flash driver struct.
 * @param err_code  0 if the asynchronous command finished gracefully. Anything
 *                  else is considered an error and will abort the operation.
 * @return error code or SPIFLASH_OK. If error code, the operation has been
 *         aborted. A call will been made to spi.async_cb signalling this.
 */
int SPIFLASH_async_trigger(spiflash_t *spi, int err_code);

#ifdef __cplusplus
}
#endif
  
#endif /*SPIFLASH_H_*/

