#include "compass_qmc5883l.h"
#include "i2c_manager.h"
#include <math.h>

// QMC5883L Register Map
namespace {
    constexpr uint8_t REG_DATA      = 0x00;  // X_LSB (6 bytes: X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB)
    constexpr uint8_t REG_STATUS    = 0x06;
    constexpr uint8_t REG_CONTROL1  = 0x09;
    constexpr uint8_t REG_CONTROL2  = 0x0A;
    constexpr uint8_t REG_SET_RESET = 0x0B;
    constexpr uint8_t REG_CHIP_ID   = 0x0D;

    // Status register bits
    constexpr uint8_t STATUS_DRDY   = 0x01;  // Data ready
    constexpr uint8_t STATUS_OVL    = 0x02;  // Overflow

    // Expected chip ID
    constexpr uint8_t EXPECTED_CHIP_ID = 0xFF;

    // Hard-iron calibration offsets
    int16_t cal_x_offset = 0;
    int16_t cal_y_offset = 0;
    int16_t cal_z_offset = 0;

    bool initialized = false;
}

namespace compass_qmc5883l {

bool begin() {
    // Verify chip ID
    uint8_t chip_id = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_CHIP_ID, chip_id)) {
        Serial.println("[COMPASS] Failed to read chip ID");
        return false;
    }
    if (chip_id != EXPECTED_CHIP_ID) {
        Serial.printf("[COMPASS] Unexpected chip ID: 0x%02X (expected 0xFF)\n", chip_id);
        return false;
    }

    // SET/RESET period (recommended by datasheet)
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_SET_RESET, 0x01)) {
        Serial.println("[COMPASS] Failed to write SET/RESET register");
        return false;
    }

    // Control Register 1: Continuous mode, 200Hz ODR, 2G range, 512 OSR
    // Bits: OSR[7:6]=00 (512), RNG[5:4]=00 (2G), ODR[3:2]=11 (200Hz), MODE[1:0]=01 (Continuous)
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL1, 0x0D)) {
        Serial.println("[COMPASS] Failed to write CONTROL1 register");
        return false;
    }

    // Control Register 2: Enable pointer rollover
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL2, 0x40)) {
        Serial.println("[COMPASS] Failed to write CONTROL2 register");
        return false;
    }

    initialized = true;
    Serial.println("[COMPASS] QMC5883L initialized (continuous, 200Hz, 2G, 512 OSR)");
    return true;
}

bool reset() {
    Serial.println("[COMPASS] Sending soft reset (CONTROL2 bit7)...");
    initialized = false;
    // Writing 0x80 to CONTROL2 triggers chip soft-reset
    i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL2, 0x80);
    delay(10);  // Datasheet: allow 5ms for reset to complete
    bool ok = begin();
    if (ok) {
        Serial.println("[COMPASS] Soft reset successful, re-initialized");
    } else {
        Serial.println("[COMPASS] Soft reset failed — device may need power cycle");
    }
    return ok;
}

bool isReady() {
    if (!initialized) return false;
    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_STATUS, status)) {
        return false;
    }
    return (status & STATUS_DRDY) != 0;
}

bool read(CompassData& out) {
    if (!initialized) return false;

    // Check status register
    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_STATUS, status)) {
        return false;
    }

    if (!(status & STATUS_DRDY)) {
        return false;  // No new data available
    }

    // Read 6 bytes of XYZ data
    uint8_t data[6];
    if (!i2c_manager::read(i2c_manager::COMPASS_DEVICE, REG_DATA, data, 6)) {
        return false;
    }

    // Parse raw values (little-endian)
    out.x_raw = (int16_t)(data[1] << 8 | data[0]);
    out.y_raw = (int16_t)(data[3] << 8 | data[2]);
    out.z_raw = (int16_t)(data[5] << 8 | data[4]);

    // Check overflow
    out.overflow = (status & STATUS_OVL) != 0;

    // Apply hard-iron calibration offsets
    int16_t cx = out.x_raw - cal_x_offset;
    int16_t cy = out.y_raw - cal_y_offset;

    // Compute heading (degrees, 0-360, magnetic north)
    float heading = atan2f((float)cy, (float)cx) * 180.0f / M_PI;
    if (heading < 0) heading += 360.0f;

    // No mounting offset needed — empirically verified with 4 cardinal points.
    // Residual ~8° error is magnetic declination in the local area (LA ≈ 11°E).

    out.heading = heading;

    out.valid = !out.overflow;
    out.last_update_ms = millis();

    return true;
}

void setCalibration(int16_t x_offset, int16_t y_offset, int16_t z_offset) {
    cal_x_offset = x_offset;
    cal_y_offset = y_offset;
    cal_z_offset = z_offset;
    Serial.printf("[COMPASS] Calibration set: X=%d Y=%d Z=%d\n", x_offset, y_offset, z_offset);
}

void getCalibration(int16_t& x_offset, int16_t& y_offset, int16_t& z_offset) {
    x_offset = cal_x_offset;
    y_offset = cal_y_offset;
    z_offset = cal_z_offset;
}

} // namespace compass_qmc5883l
