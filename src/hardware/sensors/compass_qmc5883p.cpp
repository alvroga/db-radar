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

    // Control Register 2: Set/Reset on, +/-2G range. RNG<3:2>=11 (Table 18: 00=30G,
    // 01=12G, 10=8G, 11=2G) | SET/RESET_MODE<1:0>=00 (Set and reset on) -> 0x0C.
    // +/-2G was chosen over the datasheet's own +/-8G example because Earth's horizontal
    // field here (~24.5uT) only uses ~3% of an 8G ADC's range; at 2G it's ~12%, and the
    // datasheet's own sensitivity table backs this up directly: 3750 LSB/G at 8G vs
    // 15000 LSB/G at 2G -- a real 4x resolution gain, not a marginal one.
    if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CONTROL2, 0x0C)) {
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
    Serial.println("[QMC5883P] Initialized (continuous, 200Hz, 2G, OSR1=8)");
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

// For the 'compass status' serial command (diagnostics.cpp), dispatched there via
// compass_qmc5883l::activeChip() -- kept self-contained here (rather than exposing this
// file's register constants through the header) so diagnostics.cpp doesn't need to know
// any chip's register map, just which chip is active.
void debugPrintStatus() {
    Serial.println("==== Compass Status (QMC5883P) ====");

    bool present = i2c_manager::ping(i2c_manager::COMPASS_DEVICE_QMCP);
    Serial.printf("Device (0x2C):   %s\n", present ? "DETECTED" : "NOT FOUND");
    if (!present) {
        Serial.println("Check BE-881 I2C wiring (SDA=15, SCL=7)");
        Serial.println("====================================");
        return;
    }

    uint8_t chip_id = 0;
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CHIP_ID, chip_id)) {
        Serial.printf("Chip ID:         0x%02X %s\n", chip_id,
                      chip_id == EXPECTED_CHIP_ID ? "(QMC5883P confirmed)" : "(unexpected!)");
    } else {
        Serial.println("Chip ID:         READ FAILED");
    }

    uint8_t ctrl1 = 0;
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CONTROL1, ctrl1)) {
        const char* mode_str[] = {"Suspend", "Normal", "Single", "Continuous"};
        const char* odr_str[]  = {"10Hz", "50Hz", "100Hz", "200Hz"};
        const char* osr1_str[] = {"8", "4", "2", "1"};
        Serial.printf("Mode:            %s\n", mode_str[ctrl1 & 0x03]);
        Serial.printf("Output Rate:     %s\n", odr_str[(ctrl1 >> 2) & 0x03]);
        Serial.printf("OSR1:            %s\n", osr1_str[(ctrl1 >> 4) & 0x03]);
    }

    uint8_t ctrl2 = 0;
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_CONTROL2, ctrl2)) {
        const char* rng_str[] = {"30G", "12G", "8G", "2G"};
        Serial.printf("Range:           %s\n", rng_str[(ctrl2 >> 2) & 0x03]);
    }

    uint8_t status = 0;
    if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE_QMCP, REG_STATUS, status)) {
        Serial.printf("Data Ready:      %s\n", (status & STATUS_DRDY) ? "YES" : "NO");
        Serial.printf("Overflow:        %s\n", (status & STATUS_OVFL) ? "YES" : "NO");
    }

    Serial.println("====================================");
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
