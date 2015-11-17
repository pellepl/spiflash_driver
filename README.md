# SPI flash abstraction driver

This is a hardware agnostic abstraction driver for SPI NOR flashes. It can run
either in synchronous/blocking or asynchronous/non-blocking mode.

In synchronous mode, all calls to this driver are blocking. E.g.
```SPIFLASH_write``` will not return until the data is written and the spi flash
is no longer busy. This is probably best for preemptive systems.

In asynchronous mode, the driver will return the stack when waiting for spi
communication and timeouts. This is probably best for task based systems.

This driver will 
* ensure that memory is erased using the biggest block erase range possible
* handle all page size wrapping during writes
* take care of busy polling the SR bit of the spi flash when it is busy (if wanted)

If the BUSY pin of the spi flash is wired to your processor, the driver
can handle this also and will not poll the SR. Instead, it will wait until a non busy signal is triggered.

# How to integrate

You need to configure three structs:

* ```spiflash_hal_t``` - how to communicate with the spi bus.
* ```spiflash_cmd_tbl_t``` - the commands of your spi flash (see datasheet).
* ```spiflash_config_t``` - the hardware specific parts of your spi flash (see datasheet).

## ```spiflash_hal_t```

This struct has three functions which must be defined. Following examples
are just examples, and will not compile on your platform. They are intended
to give you a hint of how to implement the HAL.

### SPI flash communication:

```int (*_spiflash_spi_txrx)(struct spiflash_s *spi, const uint8_t *tx_data, uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len)```

This function collects either a tx transaction, an rx transaction, or a tx-rx
transaction. In latter case, one must first tx, then rx.

In synchronous mode, this function would be implemented as such:

```
int impl_spiflash_spi_txrx(spiflash_t *spi, const uint8_t *tx_data,
      uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len) {
  int res = SPIFLASH_OK;
  if (tx_len > 0) {
    // first transmit tx_len bytes from tx_data if needed
    res = spi_transmit_blocking(tx_data, tx_len);
  }

  if (res == SPIFLASH_OK && rx_len > 0) {
    // then receive rx_len bytes into rx_data if needed
    res = spi_receive_blocking(rx_data, rx_len);
  }

  return res;
}
```

Asynchronously, it is a bit more awkward. Here is a pseudo implementation of
the asynchronous mode:

```
// these statics would be better handled in a struct or something
static volatile uint32_t async_tx_len;
static volatile uint8_t *async_tx_data;
static volatile uint32_t async_rx_len;
static volatile uint8_t *async_rx_data;
static volatile spiflash_t *my_spiflash_struct;

static void handle_spi_txrx(spiflash_t *spi) {
  int res = SPIFLASH_OK;
  if (async_tx_len > 0) {
    uint32_t tx_len = async_tx_len;
    async_tx_len = 0;
    // transmit tx_len bytes from tx_data, returns 0 if ok
    res = spi_transmit_nonblocking(async_tx_data, tx_len);
    // will now await IRQ if res was ok...
  } else if (async_rx_len > 0) {
    uint32_t rx_len = async_rx_len;
    async_rx_len = 0;
    // receive rx_len bytes into rx_data, returns 0 if ok
    res = spi_receive_nonblocking(async_rx_data, rx_len);
    // will now await IRQ if res was ok...
  } else {
    // async_tx_len == async_rx_len == 0, txrx transaction finished
    SPIFLASH_async_trigger(spi, res);
  }
  return res;
}

// called from lowlevel spi driver when spi rx/tx finished,
// res == 0 if all ok.
static void IRQ_callback_from_my_mcu_lowlevel_spi_driver(int res) {
  if (res == 0) {
    // async request went ok, keep going
    res = handle_spi_txrx(my_spiflash_struct);
  }
  if (res != 0) {
    // something went wrong
    SPIFLASH_async_trigger(my_spiflash_struct, res);
  }
}

int impl_spiflash_spi_txrx(spiflash_t *spi, const uint8_t *tx_data,
      uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len) {
  my_spiflash_struct = spi;
  async_tx_data = tx_data;
  async_tx_len = tx_len;
  async_rx_data = rx_data;
  async_rx_len = rx_len;
  res = handle_spi_txrx(my_spiflash_struct);
  return res;
}
```

### SPI flash CS pin handling

```void (*_spiflash_spi_cs)(struct spiflash_s *spi, uint8_t cs);```

This function simply asserts or deasserts the cs pin. Normally the CS is active
low, so an implementation could look like this:

```
void impl_spiflash_spi_cs(spiflash_t *spi, uint8_t cs) {
  if (cs) {
    // assert cs pin
    gpio_set_low(GPIO_SPI_CS_PIN);
  } else {
    // de assert cs pin
    gpio_set_high(GPIO_SPI_CS_PIN);
  }
}

```

### Timer handling

```void (*_spiflash_wait)(struct spiflash_s *spi, uint32_t ms);```

Writes and other commands to the spi flash takes time. Hence, the driver needs
to be able to pause things a bit.

In synchronous mode, an implementation be similar to this perhaps:

```
void impl_spiflash_wait(spiflash_t *spi, uint32_t ms) {
  busy_sleep(ms);
}
```

In asynchronous mode, presuming we have some kind of timer functionality on our
platform, it could look like this:

```
static void spif_timer_cb(void *user_data)  {
  SPIFLASH_async_trigger((spiflash_t *)user_data, res);
}

void impl_spiflash_wait(spiflash_t *spi, uint32_t ms) {
  // Takes a callback function, millisecond argument, and some user data void pointer.
  // Here, we use the void pointer to pass the spi flash driver struct.
  timer_call_this_given_ms_from_now(spif_timer_cb, ms, spi);
}
```

## ```spiflash_cmd_tbl_t```

This struct must contain the command bytes your specific spi flash understands.
In 99% of all spi flashes, you can simply assign this struct to ```SPIFLASH_CMD_TBL_STANDARD```
define. Otherwise, feel free to see how this define is implemented and change accordingly.
If some block erase commands are not supported, simply set them to zero. The same goes for
the fast_read command.

```
  const spiflash_cmd_tbl_t my_spiflash_cmds = SPIFLASH_CMD_TBL_STANDARD;
```

## ```spiflash_config_t```

In this struct goes the size of your spi flash, all the typical timings for writing and
erasing, page size, etc.

A typical configuration would look like this:

```
const spiflash_config_t my_spiflash_config = {
  .sz = 1024*1024*2, // e.g. for a 2 MB flash
  .page_sz = 256, // normally 256 byte pages
  .addr_sz = 3, // normally 3 byte addressing
  .addr_dummy_sz = 0, // using single line data, not quad or something
  .addr_endian = SPIFLASH_ENDIANNESS_BIG, // normally big endianess on addressing
  .sr_write_ms = 10,
  .page_program_ms = 2,
  .block_erase_4_ms = 100,
  .block_erase_8_ms = 0, // not supported
  .block_erase_16_ms = 0, // not supported
  .block_erase_32_ms = 175,
  .block_erase_64_ms = 300,
  .chip_erase_ms = 30000
};
```

### BUSY pin

If the BUSY pin of the spi flash is wired to your processor, set all timings (*_ms) in the config
to zero.

In this case, your ```impl_spiflash_wait``` will be called with argument ```ms``` being zero.
This indicates that you should wait for the BUSY pin to signal that the spi flash is not working
any longer. Do note, that in the synchronous case you must block until the flash is ready. In
the asynchronous case you must call ```SPIFLASH_async_trigger``` when the flash becomes ready.


## Finally...

Then, according to our examples above, you do:

```
static const spiflash_hal_t my_spiflash_hal = {
  ._spiflash_spi_txrx = impl_spiflash_spi_txrx,
  ._spiflash_spi_cs = impl_spiflash_spi_cs,
  ._spiflash_wait = impl_spiflash_wait;
};

static spiflash_t spif;

#ifndef I_WANT_TO_USE_SYNCHRONOUS_SPIFLASH
static void impl_spiflash_cb_async(spiflash_t *spi,
    spiflash_op_t operation, int err_code) {
  // do something with this info...
}
#endif


void init_spif(void) {
#ifdef I_WANT_TO_USE_SYNCHRONOUS_SPIFLASH
  spiflash_init(&spif,
                &my_spiflash_config,
                &my_spiflash_cmds,
                &my_spiflash_hal,
                0,
                SPIFLASH_SYNCHRONOUS,
                some_user_data_void_pointer);
#else
  spiflash_init(&spif,
                &my_spiflash_config,
                &my_spiflash_cmds,
                &my_spiflash_hal,
                impl_spiflash_cb_async,
                SPIFLASH_ASYNCHRONOUS,
                some_user_data_void_pointer);
#endif
}
```

... and you're ready to go.
