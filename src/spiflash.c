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
 * spiflash.c
 *
 * @author: petera
 */

#include "spiflash.h"

#define DECR_WAIT(_ms) ( (1 * (_ms) / 2) == 0 ? 1 : (1 * (_ms) / 2) )
#define BCW_IDLE    0
#define BCW_WAIT    1
#define BCW_READ_SR 2
#define BCW_CHECK   3

static const uint8_t MultiplyDeBruijnBitPosition[32] = {
  32, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
  31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
};

static uint8_t _spiflash_clz(uint32_t v) {
  // stolen from https://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightMultLookup
  return MultiplyDeBruijnBitPosition[((uint32_t)((v & -v) * 0x077CB531U)) >> 27];
}

static int _spiflash_is_hwbusy(spiflash_t *spi, uint8_t sr) {
  return (sr & spi->cmd_tbl->sr_busy_bit);
}

static void _spiflash_compose_address(spiflash_t *spi, uint32_t addr, uint8_t *buf) {
  uint8_t i;
  for (i = 0; i < spi->cfg->addr_sz; i++) {
    buf[i] = (spi->cfg->addr_endian ?
        ( addr >> (8*((spi->cfg->addr_sz - 1) - i)) ) :
        ( addr >> (8*i) )
        ) & 0xff;
  }
}

static void _spiflash_finalize(spiflash_t *spi) {
  spi->wait_period_ms = 0;
  spi->busy_pre_check = 0;
  spi->busy_check_wait = BCW_IDLE;
}

static uint16_t _spiflash_get_supported_block_mask(spiflash_t *spi) {
  // bit 0:256 1:512 2:1K 3:2K 4:4K 5:8K 6:16K 7:32K 8:64K etc
  uint16_t bm = 0 |
      (spi->cmd_tbl->block_erase_4 ? (1<<4) : 0) |
      (spi->cmd_tbl->block_erase_8 ? (1<<5) : 0) |
      (spi->cmd_tbl->block_erase_16 ? (1<<6) : 0) |
      (spi->cmd_tbl->block_erase_32 ? (1<<7) : 0) |
      (spi->cmd_tbl->block_erase_64 ? (1<<8) : 0);
  return bm;
}

static uint32_t _spiflash_get_largest_erase_area(spiflash_t *spi, uint32_t addr, uint32_t len) {
  uint32_t res = 0;
  uint16_t bm = _spiflash_get_supported_block_mask(spi);
  uint8_t bm_lz = _spiflash_clz(bm);
  uint8_t addr_lz = _spiflash_clz(addr);

  bm >>= bm_lz;
  bm_lz += 8; // block mask starts at 256 bytes

  // check length against smallest erase block
  if ((len & ((1 << bm_lz) - 1)) != 0) return 0;

  while (bm != 0) {
    if (addr_lz >= bm_lz && len >= (uint32_t)(1 << bm_lz)) {
      res = 1 << bm_lz;
    }

    bm >>= 1;
    bm_lz++;

    uint8_t new_bm_lz = _spiflash_clz(bm);
    if (new_bm_lz == 32) new_bm_lz =0 ;
    bm >>= new_bm_lz;
    bm_lz += new_bm_lz;
  }

  return res;
}

static uint8_t _spiflash_get_erase_cmd(spiflash_t *spi, uint32_t len) {
  if (len == 4*1024 && spi->cmd_tbl->block_erase_4)
    return spi->cmd_tbl->block_erase_4;
  else if (len == 8*1024 && spi->cmd_tbl->block_erase_8)
    return spi->cmd_tbl->block_erase_8;
  else if (len == 16*1024 && spi->cmd_tbl->block_erase_16)
    return spi->cmd_tbl->block_erase_16;
  else if (len == 32*1024 && spi->cmd_tbl->block_erase_32)
    return spi->cmd_tbl->block_erase_32;
  else if (len == 64*1024 && spi->cmd_tbl->block_erase_64)
    return spi->cmd_tbl->block_erase_64;
  else
    return 0;
}

static uint32_t _spiflash_get_erase_time(spiflash_t *spi, uint32_t len) {
  if (len == 4*1024)
    return spi->cfg->block_erase_4_ms;
  else if (len == 8*1024)
    return spi->cfg->block_erase_8_ms;
  else if (len == 16*1024)
    return spi->cfg->block_erase_16_ms;
  else if (len == 32*1024)
    return spi->cfg->block_erase_32_ms;
  else if (len == 64*1024)
    return spi->cfg->block_erase_64_ms;
  else
    return 0;
}

static int _spiflash_begin_async(spiflash_t *spi) {
  int res = SPIFLASH_OK;

  if (spi->op == SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BAD_STATE;
  }
  
  if (spi->busy_pre_check) {
    // busy check: issue read sr
    SPIF_DBG("precheck...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->read_sr, 1, &spi->sr_data, 1);
    return res;
  }
  
  switch (spi->op) {
  case SPIFLASH_OP_WRITE_sWREN: {
    // write: issue write enable
    SPIF_DBG("write - enable...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->write_enable, 1, 0, 0);
    return res;
  }
  case SPIFLASH_OP_WRITE_sADDR: {
    // write: issue write address
    SPIF_DBG("write - address...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    spi->tx_internal_buf[0] = spi->cmd_tbl->page_program;
    _spiflash_compose_address(spi, spi->addr, &spi->tx_internal_buf[1]);
    res = spi->hal->_spiflash_spi_txrx(spi,
        &spi->tx_internal_buf[0],
        1 + spi->cfg->addr_sz + spi->cfg->addr_dummy_sz,
        0, 0);
    return res;
  }
  case SPIFLASH_OP_WRITE_sDATA: {
    // write: data ordered in pages
    uint32_t rem_pg_sz = spi->cfg->page_sz - (spi->addr & (spi->cfg->page_sz - 1));
    uint32_t wr_sz = spi->wr_len < rem_pg_sz ? spi->wr_len : rem_pg_sz;
    SPIF_DBG("write - data %i of %i wait...\n", wr_sz, spi->wr_len);
    const uint8_t *wr_buf = spi->wr_buf;
    spi->wr_buf += wr_sz;
    spi->wr_len -= wr_sz;
    spi->addr += wr_sz;
    spi->wait_period_ms = spi->cfg->page_program_ms;
    spi->busy_check_wait = BCW_WAIT;
    res = spi->hal->_spiflash_spi_txrx(spi, wr_buf, wr_sz, 0, 0);
    return res;
  }

  case SPIFLASH_OP_ERASE_BLOCK_sWREN: {
    // erase: issue write enable
    SPIF_DBG("erase - enable...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->write_enable, 1, 0, 0);
    return res;
  }
  case SPIFLASH_OP_ERASE_BLOCK_sERAS: {
    // erase: issue write address
    uint32_t era_sz = _spiflash_get_largest_erase_area(spi, spi->addr, spi->erase_len);
    SPIF_DBG("erase - address %08x size %08x wait...\n", spi->addr, era_sz);
    spi->hal->_spiflash_spi_cs(spi, 1);
    uint8_t cmd = _spiflash_get_erase_cmd(spi, era_sz);
    uint32_t era_time =_spiflash_get_erase_time(spi, era_sz);
    if (cmd == 0x00) return SPIFLASH_ERR_BAD_CONFIG;
    spi->tx_internal_buf[0] = cmd;
    _spiflash_compose_address(spi, spi->addr, &spi->tx_internal_buf[1]);
    spi->addr += era_sz;
    spi->erase_len -= era_sz;
    spi->wait_period_ms = era_time;
    spi->busy_check_wait = BCW_WAIT;
    res = spi->hal->_spiflash_spi_txrx(spi,
        &spi->tx_internal_buf[0],
        1 + spi->cfg->addr_sz + spi->cfg->addr_dummy_sz,
        0, 0);
    return res;
  }

  case SPIFLASH_OP_WRITE_SR_sWREN: {
    // write_sr: issue write enable
    SPIF_DBG("write_sr - enable...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->write_enable, 1, 0, 0);
    return res;
  }
  case SPIFLASH_OP_WRITE_SR_sDATA: {
    // write_sr: data
    SPIF_DBG("write_sr - data wait...\n");
    spi->tx_internal_buf[1] = spi->sr_data;
    spi->tx_internal_buf[0] = spi->cmd_tbl->write_sr;
    spi->hal->_spiflash_spi_cs(spi, 1);
    spi->wait_period_ms = spi->cfg->sr_write_ms;
    spi->busy_check_wait = BCW_WAIT;
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->tx_internal_buf[0], 2, 0, 0);
    return res;
  }

  case SPIFLASH_OP_ERASE_CHIP_sWREN: {
    // erase chip: issue write enable
    SPIF_DBG("erase chip - enable...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->write_enable, 1, 0, 0);
    return res;
  }
  case SPIFLASH_OP_ERASE_CHIP_sERAS: {
    // erase chip: cmd
    SPIF_DBG("erase chip - command wait...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    spi->tx_internal_buf[0] = spi->cmd_tbl->chip_erase;
    spi->wait_period_ms = spi->cfg->chip_erase_ms;
    spi->busy_check_wait = BCW_WAIT;
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->tx_internal_buf[0], 1, 0, 0);
    return res;
  }

  case SPIFLASH_OP_READ: {
    // read: issue address and read
    SPIF_DBG("read - address and data...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    spi->tx_internal_buf[0] = spi->cmd_tbl->read_data;
    _spiflash_compose_address(spi, spi->addr, &spi->tx_internal_buf[1]);

    res = spi->hal->_spiflash_spi_txrx(spi,
        &spi->tx_internal_buf[0],
        1 + spi->cfg->addr_sz + spi->cfg->addr_dummy_sz,
        spi->rd_buf, spi->rd_len);
    return res;
  }

  case SPIFLASH_OP_FAST_READ: {
    // fast read: issue address and read
    SPIF_DBG("read fast - address and data...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    spi->tx_internal_buf[0] = spi->cmd_tbl->read_data_fast;
    _spiflash_compose_address(spi, spi->addr, &spi->tx_internal_buf[1]);
    spi->tx_internal_buf[1 + spi->cfg->addr_sz + 1] = 0; // dummy for fast read
    res = spi->hal->_spiflash_spi_txrx(spi,
        &spi->tx_internal_buf[0],
        1 + spi->cfg->addr_sz + 1 + spi->cfg->addr_dummy_sz,
        spi->rd_buf, spi->rd_len);
    return res;
  }

  case SPIFLASH_OP_READ_JEDEC: {
    // read_jedec
    SPIF_DBG("read_jedec...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->jedec_id, 1, (uint8_t *)spi->id_dst, 3);
    return res;
  }

  case SPIFLASH_OP_READ_PRODUCT: {
    // read_jedec
    SPIF_DBG("read_prod...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->device_id, 1, (uint8_t *)spi->id_dst, 3);
    return res;
  }

  case SPIFLASH_OP_READ_SR:
  case SPIFLASH_OP_READ_SR_BUSY: {
    // read_sr
    SPIF_DBG("read_sr...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->read_sr, 1, (uint8_t *)spi->sr_dst, 1);
    return res;
  }

  case SPIFLASH_OP_READ_REG: {
    // read_reg
    SPIF_DBG("read_reg...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->reg_nbr, 1, (uint8_t *)spi->reg_dst, 1);
    return res;
  }

  case SPIFLASH_OP_WRITE_REG_sWREN: {
    // write_reg: issue write enable
    SPIF_DBG("write_reg - enable...\n");
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->write_enable, 1, 0, 0);
    return res;
  }
  case SPIFLASH_OP_WRITE_REG_DATA:
  case SPIFLASH_OP_WRITE_REG_sDATAWAIT: {
    // write_reg: data
    SPIF_DBG("write_reg - data%s...\n", spi->op == SPIFLASH_OP_WRITE_REG_DATA ? "" : " wait");
    spi->hal->_spiflash_spi_cs(spi, 1);
    spi->busy_check_wait = spi->op == SPIFLASH_OP_WRITE_REG_DATA ? BCW_IDLE : BCW_WAIT;
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->tx_internal_buf[0], 2, 0, 0);
    return res;
  }

  case SPIFLASH_OP_IDLE:
  default:
    res = SPIFLASH_ERR_INTERNAL;
    break;
  } // switch (spi->op)

  return res;
}



static int _spiflash_end_async(spiflash_t *spi, int res) {
  // handle early termination
  if (res != SPIFLASH_OK) {
    _spiflash_finalize(spi);
    return res;
  }
  
  // handle busy pre check
  if (spi->busy_pre_check) {
    if (_spiflash_is_hwbusy(spi, spi->sr_data)) {
      spi->hal->_spiflash_spi_cs(spi, 0);
      SPIF_DBG("precheck busy\n");
      return SPIFLASH_ERR_HW_BUSY;
    } else {
      SPIF_DBG("precheck ok\n");
      spi->busy_pre_check = 0;
      return _spiflash_begin_async(spi);
    }
  }
  
  // handle busy-check-wait states
  switch (spi->busy_check_wait) {
  case BCW_WAIT:
    SPIF_DBG("busy check WAIT %i...\n", spi->wait_period_ms);
    spi->hal->_spiflash_spi_cs(spi, 0);
    // if wait period is 0, call wait and then break free of the bsw loop
    spi->busy_check_wait = spi->wait_period_ms == 0 ? BCW_IDLE : BCW_READ_SR;
    spi->hal->_spiflash_wait(spi, spi->wait_period_ms);
    return SPIFLASH_OK;
  case BCW_READ_SR:
    SPIF_DBG("busy CHECK wait...\n");
    spi->busy_check_wait = BCW_CHECK;
    spi->hal->_spiflash_spi_cs(spi, 1);
    res = spi->hal->_spiflash_spi_txrx(spi, &spi->cmd_tbl->read_sr, 1, &spi->sr_data, 1);
    return res;
  case BCW_CHECK:
    spi->hal->_spiflash_spi_cs(spi, 0);
    if (_spiflash_is_hwbusy(spi, spi->sr_data)) {
      spi->wait_period_ms = DECR_WAIT(spi->wait_period_ms);
      SPIF_DBG("BUSY check WAIT %i...\n", spi->wait_period_ms);
      spi->busy_check_wait = BCW_READ_SR;
      spi->hal->_spiflash_wait(spi, spi->wait_period_ms);
      return SPIFLASH_OK;
    } else {
      SPIF_DBG("busy check wait ok\n");
      spi->busy_check_wait = BCW_IDLE;
      break;
    }
  case BCW_IDLE:
    SPIF_DBG("no BCW\n");
    break;
  } // switch (spi->busy_check_wait)
  
  // handle results
  switch (spi->op) {
  case SPIFLASH_OP_WRITE_sWREN:
    SPIF_DBG("write - enable ok\n");
    spi->hal->_spiflash_spi_cs(spi, 0);
    spi->op = SPIFLASH_OP_WRITE_sADDR;
    break;
  case SPIFLASH_OP_WRITE_sADDR:
    SPIF_DBG("write - address ok\n");
    spi->op = SPIFLASH_OP_WRITE_sDATA;
    break;
  case SPIFLASH_OP_WRITE_sDATA:
    if (spi->wr_len == 0) {
      SPIF_DBG("write - data ok, finish\n");
      spi->op = SPIFLASH_OP_IDLE;
    } else {
      SPIF_DBG("write - data ok, new chunk\n");
      spi->op = SPIFLASH_OP_WRITE_sWREN;
    }
    break;

  case SPIFLASH_OP_ERASE_BLOCK_sWREN:
    SPIF_DBG("erase - enable ok\n");
    spi->hal->_spiflash_spi_cs(spi, 0);
    spi->op = SPIFLASH_OP_ERASE_BLOCK_sERAS;
    break;
  case SPIFLASH_OP_ERASE_BLOCK_sERAS:
    SPIF_DBG("erase - ok\n");
    if (spi->erase_len == 0) {
      SPIF_DBG("erase - ok, finish\n");
      spi->op = SPIFLASH_OP_IDLE;
    } else {
      SPIF_DBG("erase - ok, new chunk\n");
      spi->op = SPIFLASH_OP_ERASE_BLOCK_sWREN;
    }
    break;

  case SPIFLASH_OP_WRITE_SR_sWREN:
    SPIF_DBG("write_sr - enable ok\n");
    spi->hal->_spiflash_spi_cs(spi, 0);
    spi->op = SPIFLASH_OP_WRITE_SR_sDATA;
    break;
  case SPIFLASH_OP_WRITE_SR_sDATA:
    SPIF_DBG("write_sr - ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_ERASE_CHIP_sWREN:
    SPIF_DBG("erase chip - enable ok\n");
    spi->hal->_spiflash_spi_cs(spi, 0);
    spi->op = SPIFLASH_OP_ERASE_CHIP_sERAS;
    break;
  case SPIFLASH_OP_ERASE_CHIP_sERAS:
    SPIF_DBG("erase chip - ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_READ:
    SPIF_DBG("read - ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_FAST_READ:
    SPIF_DBG("fast read - ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_READ_JEDEC:
    SPIF_DBG("read jedec ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_READ_PRODUCT:
    SPIF_DBG("read prod ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_READ_SR_BUSY:
  case SPIFLASH_OP_READ_SR:
    SPIF_DBG("read sr ok\n");
    if (spi->op == SPIFLASH_OP_READ_SR_BUSY) {
      *spi->sr_dst = ((*spi->sr_dst & spi->cmd_tbl->sr_busy_bit)) != 0;
    }
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_WRITE_REG_sWREN:
    SPIF_DBG("write_reg - enable ok\n");
    spi->hal->_spiflash_spi_cs(spi, 0);
    spi->op = SPIFLASH_OP_WRITE_REG_sDATAWAIT;
    break;
  case SPIFLASH_OP_WRITE_REG_sDATAWAIT:
  case SPIFLASH_OP_WRITE_REG_DATA:
    SPIF_DBG("write_reg - ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_READ_REG:
    SPIF_DBG("read reg ok\n");
    spi->op = SPIFLASH_OP_IDLE;
    break;

  case SPIFLASH_OP_IDLE:
  default:
    res = SPIFLASH_ERR_INTERNAL;
    break;

  } // switch (spi->op)

  if (res == SPIFLASH_OK && spi->op != SPIFLASH_OP_IDLE) {
    // moar to do
    res = _spiflash_begin_async(spi);
  } else {
    // finished or error
    spi->hal->_spiflash_spi_cs(spi, 0);
    _spiflash_finalize(spi);
  }
  

  return res;
}


static int _spiflash_exe(spiflash_t *spi) {
  int res = SPIFLASH_OK;

  if (spi->could_be_busy) {
    spi->busy_pre_check = 1;
  }

  if (spi->async) {
    // asynchronous
    res = _spiflash_begin_async(spi);
  } else {
    // blocking
    res = _spiflash_begin_async(spi);
    while (res == SPIFLASH_OK && spi->op != SPIFLASH_OP_IDLE) {
      res = SPIFLASH_async_trigger(spi, res);
    }
    _spiflash_finalize(spi);
  }

  return res;
}


void SPIFLASH_init(spiflash_t *spi, 
                   const spiflash_config_t *cfg,
                   const spiflash_cmd_tbl_t *cmd,
                   const spiflash_hal_t *hal,
                   spiflash_cb_async_t async_cb,
                   uint8_t async,
                   void *user_data) {
  memset(spi, 0, sizeof(spiflash_t));
  spi->cfg = cfg;
  spi->cmd_tbl = cmd;
  spi->hal = hal;
  spi->async_cb = async_cb;
  
  spi->async = async;
  spi->user_data = user_data;
  spi->op = SPIFLASH_OP_IDLE;
}


int SPIFLASH_async_trigger(spiflash_t *spi, int err_code) {
  int res = _spiflash_end_async(spi, err_code);
  spiflash_op_t op = spi->op;
  if (res != SPIFLASH_OK || op == SPIFLASH_OP_IDLE) {
    if (res != SPIFLASH_OK) {
      spi->op = SPIFLASH_OP_IDLE;
    }
    if (spi->async && spi->async_cb) {
      spi->async_cb(spi, op, res);
    }
  }
  return res;
}

int SPIFLASH_write(spiflash_t *spi, uint32_t addr, uint32_t len, const uint8_t *buf) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }
  
  spi->addr = addr;
  spi->wr_buf = buf;
  spi->wr_len = len;
  
  spi->op = SPIFLASH_OP_WRITE_sWREN;
  
  res = _spiflash_exe(spi);
  
  return res;
}

int SPIFLASH_read(spiflash_t *spi, uint32_t addr, uint32_t len, uint8_t *buf) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->addr = addr;
  spi->rd_buf = buf;
  spi->rd_len = len;

  spi->op = SPIFLASH_OP_READ;

  res = _spiflash_exe(spi);

  return res;
}

int SPIFLASH_fast_read(spiflash_t *spi, uint32_t addr, uint32_t len,
                       uint8_t *buf) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->addr = addr;
  spi->rd_buf = buf;
  spi->rd_len = len;

  spi->op = spi->cmd_tbl->read_data_fast ? SPIFLASH_OP_FAST_READ : SPIFLASH_OP_READ;

  res = _spiflash_exe(spi);

  return res;
}


int SPIFLASH_read_jedec_id(spiflash_t *spi, uint32_t *jedec_id) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }
  
  spi->id_dst = jedec_id;
  
  spi->op = SPIFLASH_OP_READ_JEDEC;
  
  res = _spiflash_exe(spi);
  
  return res;
}

int SPIFLASH_read_product_id(spiflash_t *spi, uint32_t *prod_id) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->id_dst = prod_id;

  spi->op = SPIFLASH_OP_READ_PRODUCT;

  res = _spiflash_exe(spi);

  return res;
}

int SPIFLASH_read_sr(spiflash_t *spi, uint8_t *sr) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->sr_dst = sr;

  spi->op = SPIFLASH_OP_READ_SR;

  res = _spiflash_exe(spi);

  return res;
}

int SPIFLASH_read_sr_busy(spiflash_t *spi, uint8_t *busy) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->sr_dst = busy;

  spi->op = SPIFLASH_OP_READ_SR_BUSY;

  res = _spiflash_exe(spi);

  return res;
}


int SPIFLASH_write_sr(spiflash_t *spi, uint8_t sr) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->sr_data = sr;

  spi->op = SPIFLASH_OP_WRITE_SR_sWREN;

  res = _spiflash_exe(spi);

  return res;
}


int SPIFLASH_read_reg(spiflash_t *spi, uint8_t reg, uint8_t *data) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->reg_nbr = reg;
  spi->reg_dst = data;

  spi->op = SPIFLASH_OP_READ_REG;

  res = _spiflash_exe(spi);

  return res;
}


int SPIFLASH_write_reg(spiflash_t *spi, uint8_t reg, uint8_t data,
    uint8_t write_en, uint32_t wait_ms) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->tx_internal_buf[0] = reg;
  spi->tx_internal_buf[1] = data;

  spi->op = write_en ? SPIFLASH_OP_WRITE_REG_sWREN : SPIFLASH_OP_WRITE_REG_DATA;
  if (write_en) {
    spi->wait_period_ms = wait_ms;
  }

  res = _spiflash_exe(spi);

  return res;
}


int SPIFLASH_erase(spiflash_t *spi, uint32_t addr, uint32_t len) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  uint32_t era_sz = _spiflash_get_largest_erase_area(spi, addr, len);

  if (era_sz == 0) {
    return SPIFLASH_ERR_ERASE_UNALIGNED;
  }

  spi->addr = addr;
  spi->erase_len = len;

  spi->op = SPIFLASH_OP_ERASE_BLOCK_sWREN;

  res = _spiflash_exe(spi);

  return res;
}

int SPIFLASH_chip_erase(spiflash_t *spi) {
  int res;
  if (spi->op != SPIFLASH_OP_IDLE) {
    return SPIFLASH_ERR_BUSY;
  }

  spi->op = SPIFLASH_OP_ERASE_CHIP_sWREN;

  res = _spiflash_exe(spi);

  return res;
}

int SPIFLASH_is_busy(spiflash_t *spi) {
  return spi->op == SPIFLASH_OP_IDLE ? SPIFLASH_OK : SPIFLASH_ERR_BUSY;
}

