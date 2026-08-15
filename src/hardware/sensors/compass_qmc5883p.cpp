#include "compass_qmc5883p.h"
#include "i2c_manager.h"

// QMC5883P Register Map (QST datasheet 13-52-19 Rev. A) — NOT the same layout as the
// QMC5883L despite the sibling name: chip ID lives at 0x00 (not 0x0D), status at 0x09
// (not 0x06), and mode/ODR/range are packed into two control registers differently.
namespace {
    constexpr uint8_t REG_CHIP_ID   = 0x00;
    constexpr uint8_t REG_DATA      = 0x01;  // X_LSB (6 bytes: X_LSB,X_MSB,Y_LSB,Y_MSB,Z_LSB,Z_MSB)
    constexpr uint8_t REG_STATUS    = 0x09;
    constexpr uint8_t REG_SIGN      = 0x29;  // Undocumented in the register map table, but
                                              // required by the datasheet's own §7.1/7.2/7.3
                                              // setup examples ("Define the sign for X Y and Z
                                              // axis") before every mode change.
    constexpr uint8_t REG_CONTROL1  = 0x0A;  // OSR2<7:6> OSR1<5:4> ODR<3:2> MODE<1:0>
    constexpr uint8_t REG_CONTROL2  = 0x0B;  // SOFT_RST<7> SELF_TEST<6> - - RNG<3:2> SET/RESET<1:0>

    constexpr uint8_t STATUS_DRDY = 0x01;
    constexpr uint8_t STATUS_OVFL = 0x02;

    constexpr uint8_t EXPECTED_CHIP_ID = 0x80;

    bool initialized = false;
}

namespace compass_qmc5883p {

bool begin() {
    initialized = false;

    uint8_t chip_id = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CHIP_ID, chip_id)) {
        Serial.println("[QMC5883P] Failed to read chip ID");
        return false;
    }
    if (chip_id != EXPECTED_CHIP_ID) {
        Serial.printf("[QMC5883P] Unexpected chip ID: 0x%02X (expected 0x80)\n", chip_id);
        return false;
    }

    // Axis sign definition — datasheet's own setup examples write this before every
    // mode change (§7.1-7.3); not documented in the register map table but load-bearing.
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_SIGN, 0x06)) {
        Serial.println("[QMC5883P] Failed to write sign register");
        return false;
    }

    // Control Register 2: Set/Reset on, +/-8G range (0x08 — matches the datasheet's own
    // continuous-mode example exactly).
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CONTROL2, 0x08)) {
        Serial.println("[QMC5883P] Failed to write CONTROL2 register");
        return false;
    }

    // Control Register 1: OSR2=1x (no downsample), OSR1=8x (lowest noise), ODR=200Hz,
    // MODE=continuous. Bits: OSR2[7:6]=00, OSR1[5:4]=00, ODR[3:2]=11, MODE[1:0]=11 -> 0x0F.
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CONTROL1, 0x0F)) {
        Serial.println("[QMC5883P] Failed to write CONTROL1 register");
        return false;
    }

    initialized = true;
    Serial.println("[QMC5883P] Initialized (continuous, 200Hz, 8G, OSR1=8)");
    return true;
}

bool reset() {
    Serial.println("[QMC5883P] Sending soft reset (CONTROL2 bit7)...");
    initialized = false;
    i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CONTROL2, 0x80);
    delay(10);  // Datasheet POR completion time is <=250us; matches the QMC5883L's own margin
    bool ok = begin();
    Serial.println(ok ? "[QMC5883P] Re-init successful" : "[QMC5883P] Re-init failed — device may need power cycle");
    return ok;
}

bool isReady() {
    if (!initialized) return false;
    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_STATUS, status)) {
        return false;
    }
    return (status & STATUS_DRDY) != 0;
}

bool readRaw(int16_t& x, int16_t& y, int16_t& z, bool& overflow) {
    if (!initialized) return false;

    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_STATUS, status)) {
        return false;
    }
    if (!(status & STATUS_DRDY)) {
        return false;  // No new data available
    }

    // Native burst order is X,Y,Z, little-endian -- same layout as the QMC5883L, unlike
    // the HMC5883L's X,Z,Y big-endian burst.
    uint8_t data[6];
    if (!i2c_manager::read(i2c_manager::COMPASS_DEVICE_QMCP, REG_DATA, data, 6)) {
        return false;
    }

    x = (int16_t)(data[1] << 8 | data[0]);
    y = (int16_t)(data[3] << 8 | data[2]);
    z = (int16_t)(data[5] << 8 | data[4]);

    overflow = (status & STATUS_OVFL) != 0;

    return true;
}

} // namespace compass_qmc5883p
