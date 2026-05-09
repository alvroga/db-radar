#pragma once
#include "core/arduino_compat.h"

// Basic CST820 (and similar CST8xx) touch driver
// I2C address default: 0x15

struct CST820Point {
  uint16_t x;   // raw X from controller
  uint16_t y;   // raw Y from controller
  bool     pressed;
};

// Call once (after Wire.begin). Optionally pass a reset lambda if you want.
bool cst820_begin(uint8_t i2c_addr = 0x15);

// Returns true if read succeeded. 'pt.pressed' tells if finger is down.
// Raw ranges vary by firmware (could be ~0..480, ~0..1023, or ~0..4095)
bool cst820_read(CST820Point &pt, uint8_t i2c_addr = 0x15);