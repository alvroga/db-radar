#include "compass_hmc5883l.h"
#include "i2c_manager.h"

// HMC5883L Register Map
namespace {
    constexpr uint8_t REG_CONFIG_A   = 0x00;
    constexpr uint8_t REG_CONFIG_B   = 0x01;
    constexpr uint8_t REG_MODE       = 0x02;
    constexpr uint8_t REG_DATA_X_MSB = 0x03;  // burst: X_MSB,X_LSB,Z_MSB,Z_LSB,Y_MSB,Y_LSB
    constexpr uint8_t REG_STATUS     = 0x09;
    constexpr uint8_t REG_ID_A       = 0x0A;
    constexpr uint8_t REG_ID_B       = 0x0B;
    constexpr uint8_t REG_ID_C       = 0x0C;

    constexpr uint8_t STATUS_RDY = 0x01;

    // Fixed identification bytes ('H', '4', '3') — this chip's analog to the
    // QMC5883L's single 0xFF chip-ID byte.
    constexpr uint8_t EXPECTED_ID_A = 0x48;
    constexpr uint8_t EXPECTED_ID_B = 0x34;
    constexpr uint8_t EXPECTED_ID_C = 0x33;

    // A data register reading exactly this value means that axis' ADC saturated
    // (datasheet: -4096, "measurement is out of range"), not a genuine reading.
    constexpr int16_t OVERFLOW_SENTINEL = -4096;

    bool initialized = false;
}

namespace compass_hmc5883l {

bool begin() {
    uint8_t id_a = 0, id_b = 0, id_c = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_ID_A, id_a) ||
        !i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_ID_B, id_b) ||
        !i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_ID_C, id_c)) {
        Serial.println("[HMC5883L] Failed to read identification registers");
        return false;
    }
    if (id_a != EXPECTED_ID_A || id_b != EXPECTED_ID_B || id_c != EXPECTED_ID_C) {
        Serial.printf("[HMC5883L] Unexpected ID: 0x%02X 0x%02X 0x%02X (expected 0x48 0x34 0x33)\n",
                      id_a, id_b, id_c);
        return false;
    }

    // Config A: 8-sample averaging, 15Hz output rate, normal measurement mode.
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_HMC, REG_CONFIG_A, 0x70)) {
        Serial.println("[HMC5883L] Failed to write Config Register A");
        return false;
    }
    // Config B: gain = +/-1.3 Ga (1090 LSB/Gauss) -- datasheet default.
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_HMC, REG_CONFIG_B, 0x20)) {
        Serial.println("[HMC5883L] Failed to write Config Register B");
        return false;
    }
    // Mode: continuous measurement.
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_HMC, REG_MODE, 0x00)) {
        Serial.println("[HMC5883L] Failed to write Mode Register");
        return false;
    }

    initialized = true;
    Serial.println("[HMC5883L] Initialized (continuous, 15Hz, +/-1.3Ga)");
    return true;
}

bool isReady() {
    if (!initialized) return false;
    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_STATUS, status)) {
        return false;
    }
    return (status & STATUS_RDY) != 0;
}

// For the 'compass status' serial command (diagnostics.cpp), dispatched there via
// compass_qmc5883l::activeChip() -- see the identical comment in compass_qmc5883p.cpp's
// debugPrintStatus() for why this lives here instead of diagnostics.cpp. Register bytes
// are shown raw (not bit-decoded like the QMC5883P/QMC5883L versions) since this file
// doesn't carry a verified bit-field table for CONFIG_A/CONFIG_B beyond the two values
// begin() itself writes -- decoding them here would risk asserting something unverified.
void debugPrintStatus() {
    Serial.println("==== Compass Status (HMC5883L) ====");

    bool present = i2c_manager::ping(i2c_manager::COMPASS_DEVICE_HMC);
    Serial.printf("Device (0x1E):   %s\n", present ? "DETECTED" : "NOT FOUND");
    if (!present) {
        Serial.println("Check BN-880 I2C wiring (SDA=15, SCL=7)");
        Serial.println("====================================");
        return;
    }

    uint8_t id_a = 0, id_b = 0, id_c = 0;
    bool id_ok = i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_ID_A, id_a) &&
                 i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_ID_B, id_b) &&
                 i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_ID_C, id_c);
    if (id_ok) {
        bool match = (id_a == EXPECTED_ID_A && id_b == EXPECTED_ID_B && id_c == EXPECTED_ID_C);
        Serial.printf("Chip ID:         0x%02X 0x%02X 0x%02X %s\n", id_a, id_b, id_c,
                      match ? "('H43' confirmed)" : "(unexpected!)");
    } else {
        Serial.println("Chip ID:         READ FAILED");
    }

    uint8_t cfg_a = 0, cfg_b = 0, mode = 0;
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_CONFIG_A, cfg_a)) {
        Serial.printf("Config A:        0x%02X (expect 0x70: 8-sample avg, 15Hz)\n", cfg_a);
    }
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_CONFIG_B, cfg_b)) {
        Serial.printf("Config B:        0x%02X (expect 0x20: +/-1.3Ga gain)\n", cfg_b);
    }
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_MODE, mode)) {
        Serial.printf("Mode:            0x%02X (expect 0x00: continuous)\n", mode);
    }

    uint8_t status = 0;
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_STATUS, status)) {
        Serial.printf("Data Ready:      %s\n", (status & STATUS_RDY) ? "YES" : "NO");
    }
    Serial.println("(No hardware overflow bit on this chip -- readRaw() infers it from the");
    Serial.println(" -4096 ADC-saturation sentinel on any axis, see 'compass read'.)");

    Serial.println("====================================");
}

bool readRaw(int16_t& x, int16_t& y, int16_t& z, bool& overflow) {
    if (!initialized) return false;

    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_HMC, REG_STATUS, status)) {
        return false;
    }
    if (!(status & STATUS_RDY)) {
        return false;  // No new data available
    }

    // Burst order is X, Z, Y (NOT X,Y,Z like the QMC5883L), and each axis is
    // big-endian/MSB-first (the opposite of the QMC5883L's little-endian burst).
    // Both are the classic HMC5883L porting mistakes -- deliberately not copy-pasted
    // from compass_qmc5883l.cpp's decode.
    uint8_t data[6];
    if (!i2c_manager::read(i2c_manager::COMPASS_DEVICE_HMC, REG_DATA_X_MSB, data, 6)) {
        return false;
    }

    x = (int16_t)((data[0] << 8) | data[1]);
    z = (int16_t)((data[2] << 8) | data[3]);
    y = (int16_t)((data[4] << 8) | data[5]);

    overflow = (x == OVERFLOW_SENTINEL) || (y == OVERFLOW_SENTINEL) || (z == OVERFLOW_SENTINEL);

    return true;
}

} // namespace compass_hmc5883l
