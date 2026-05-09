#pragma once
#include "core/arduino_compat.h"

struct CompassData {
    int16_t x_raw = 0, y_raw = 0, z_raw = 0;
    float heading = NAN;       // degrees 0-360, magnetic north
    bool valid = false;
    bool overflow = false;     // sensor saturation flag
    uint32_t last_update_ms = 0;
};

namespace compass_qmc5883l {

    bool begin();                    // Init sensor: SET/RESET, continuous mode, 200Hz, 2G, 512 OSR
    bool reset();                    // Soft-reset chip then re-run begin() (recovers from I2C bus collisions)
    bool read(CompassData& out);     // Read XYZ, compute heading, check status
    bool isReady();                  // Check data-ready bit in STATUS register

    // Calibration (store hard-iron offsets)
    void setCalibration(int16_t x_offset, int16_t y_offset, int16_t z_offset);
    void getCalibration(int16_t& x_offset, int16_t& y_offset, int16_t& z_offset);

}
