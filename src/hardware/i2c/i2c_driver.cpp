// I2C_Driver.cpp - Compatibility layer redirecting to i2c_manager
#include "i2c_driver.h"
#include "i2c_manager.h"

void I2C_Init() {
  // I2C initialization is now handled by i2c_manager in device_manager
  // This function is kept for compatibility with vendor code
}

bool I2C_Read(uint8_t dev, uint8_t reg, uint8_t* data, size_t len) {
  // Create device handle on-the-fly based on address
  i2c_manager::DeviceHandle device = {dev, "VENDOR"};
  return i2c_manager::read(device, reg, data, len);
}

bool I2C_Write(uint8_t dev, uint8_t reg, const uint8_t* data, size_t len) {
  // Create device handle on-the-fly based on address
  i2c_manager::DeviceHandle device = {dev, "VENDOR"};
  return i2c_manager::write(device, reg, data, len);
}