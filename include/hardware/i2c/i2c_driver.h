#pragma once
#include "core/arduino_compat.h"

// Return true on success, false on failure
void I2C_Init();  // OK if empty (Wire is already begun elsewhere)

bool I2C_Read(uint8_t dev, uint8_t reg, uint8_t* data, size_t len);
bool I2C_Write(uint8_t dev, uint8_t reg, const uint8_t* data, size_t len);