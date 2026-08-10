#pragma once
#include <stdint.h>
#include "driver/spi_master.h"

// Initialize ST7701S over the given SPI device (already added to SPI bus).
// Expects the SPI device to be configured with:
//   command_bits = 1, address_bits = 8, mode = 0, spics_io_num = -1 (we toggle CS externally)
// Returns true on success.
bool st7701_init(spi_device_handle_t spi);

// (Optional) low-level helpers if you ever need custom commands.
bool st7701_send_cmd(spi_device_handle_t spi, uint8_t cmd);
bool st7701_send_data(spi_device_handle_t spi, uint8_t data);