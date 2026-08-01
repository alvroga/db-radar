#include "accel_qmi8658.h"
#include "i2c_manager.h"
#include <math.h>

// Register map taken from include/hardware/sensors/gyro_qmi8658.h (the vendor
// header). The vendor *driver* is deliberately not reused: it enables accel and
// gyro together (CTRL7 = 0x43), keeps state in file-scope globals, and is flagged
// in CLAUDE.md as needing a C++ refactor. It also has no callers and is not in the
// build. What it does get right -- routing through i2c_driver.h, which forwards to
// i2c_manager and so inherits the bus mutex and retry logic -- is preserved here by
// calling i2c_manager directly.

namespace {
    constexpr uint8_t REG_WHO_AM_I  = 0x00;
    constexpr uint8_t REG_CTRL1     = 0x02;  // serial interface / oscillator
    constexpr uint8_t REG_CTRL2     = 0x03;  // accel range + ODR
    constexpr uint8_t REG_CTRL3     = 0x04;  // gyro range + ODR
    constexpr uint8_t REG_CTRL5     = 0x06;  // low-pass filters
    constexpr uint8_t REG_CTRL6     = 0x07;  // AttitudeEngine (unused)
    constexpr uint8_t REG_CTRL7     = 0x08;  // sensor enable
    constexpr uint8_t REG_AX_L      = 0x35;  // accel X..Z then gyro X..Z, contiguous to 0x40

    constexpr uint8_t EXPECTED_WHO_AM_I = 0x05;

    // CTRL7 bits: bit0 aEN, bit1 gEN.
    constexpr uint8_t CTRL7_ACCEL_ONLY  = 0x01;
    constexpr uint8_t CTRL7_ACCEL_GYRO  = 0x03;
    constexpr uint8_t CTRL7_POWER_DOWN  = 0x00;

    // Accel: +/-4 G, 250 Hz ODR.
    //
    // 4 G not 2 G: gravity is 1 G and walking adds +/-0.3-0.5 G of step
    // acceleration, so 2 G leaves almost no headroom for a sharp movement and a
    // clipped sample is silently wrong rather than obviously wrong.
    //
    // 250 Hz because the sensor must comfortably outrun both read rates that will
    // be used -- 10 Hz from the System Task, and 100 Hz for field sample 9.
    constexpr uint8_t CTRL2_ACCEL = (0x1 << 4) | 0x5;   // aFS=4G, aODR=250Hz
    constexpr float   ACCEL_LSB_PER_G = 8192.0f;        // 32768 / 4

    // Gyro: +/-256 dps, 250 Hz. Hand rotation never approaches 256 dps.
    constexpr uint8_t CTRL3_GYRO = (0x4 << 4) | 0x5;    // gFS=256dps, gODR=250Hz
    constexpr float   GYRO_LSB_PER_DPS = 128.0f;        // 32768 / 256

    // LPF mode 3 = 13.37% of ODR ~= 33 Hz at 250 Hz ODR.
    //
    // ⚠️ This is chosen for sample 9, not for the 10 Hz path. At 100 Hz sampling a
    // 33 Hz cutoff preserves the body-shake spectrum and its harmonics, which is
    // the entire point of that sample. At 10 Hz sampling, Nyquist is 5 Hz, so
    // content between 5 and 33 Hz ALIASES into the band we care about. That is a
    // real defect for a gravity estimate derived from 10 Hz data, and it is
    // deliberate: narrowing the filter to suit 10 Hz would destroy the one
    // measurement that tells us whether 10 Hz is adequate at all. Revisit once
    // sample 9 is analysed (WP-3) -- if 10 Hz is kept, drop to LPF mode 0.
    constexpr uint8_t CTRL5_LPF = (0x6 << 4) | 0x6 | 0x11;  // gLPF+aLPF mode 3, both enabled

    bool     g_initialized = false;
    bool     g_enabled     = true;
    bool     g_gyro_on     = false;
    uint8_t  g_addr        = 0;
    i2c_manager::DeviceHandle* g_dev = nullptr;

    bool writeReg(uint8_t reg, uint8_t val) {
        return g_dev && i2c_manager::writeByte(*g_dev, reg, val);
    }

    bool probe(i2c_manager::DeviceHandle& dev) {
        uint8_t who = 0;
        if (!i2c_manager::readByte(dev, REG_WHO_AM_I, who)) return false;
        if (who != EXPECTED_WHO_AM_I) {
            Serial.printf("[ACCEL] 0x%02X answered but WHO_AM_I=0x%02X (expected 0x%02X)\n",
                          dev.addr, who, EXPECTED_WHO_AM_I);
            return false;
        }
        return true;
    }
}

namespace accel_qmi8658 {

bool begin(bool enable_gyro) {
    g_initialized = false;
    g_gyro_on = enable_gyro;

    // Probe both addresses. Which one is populated depends on the SDO strap and
    // has never been established for this board, so do not assume.
    if (probe(i2c_manager::IMU_DEVICE_LOW)) {
        g_dev = &i2c_manager::IMU_DEVICE_LOW;
    } else if (probe(i2c_manager::IMU_DEVICE_HIGH)) {
        g_dev = &i2c_manager::IMU_DEVICE_HIGH;
    } else {
        Serial.println("[ACCEL] QMI8658 not found at 0x6A or 0x6B");
        g_dev = nullptr;
        g_addr = 0;
        return false;
    }
    g_addr = g_dev->addr;

    // CTRL1: clear bit0 to enable the internal oscillator, set bit6 for address
    // auto-increment (required for the multi-byte burst read below).
    uint8_t ctrl1 = 0;
    if (!i2c_manager::readByte(*g_dev, REG_CTRL1, ctrl1)) {
        Serial.println("[ACCEL] Failed to read CTRL1");
        return false;
    }
    ctrl1 &= 0xFE;
    ctrl1 |= 0x40;
    if (!writeReg(REG_CTRL1, ctrl1))            { Serial.println("[ACCEL] CTRL1 write failed"); return false; }
    if (!writeReg(REG_CTRL2, CTRL2_ACCEL))      { Serial.println("[ACCEL] CTRL2 write failed"); return false; }
    if (enable_gyro && !writeReg(REG_CTRL3, CTRL3_GYRO)) {
        Serial.println("[ACCEL] CTRL3 write failed"); return false;
    }
    if (!writeReg(REG_CTRL5, CTRL5_LPF))        { Serial.println("[ACCEL] CTRL5 write failed"); return false; }
    if (!writeReg(REG_CTRL6, 0x00))             { Serial.println("[ACCEL] CTRL6 write failed"); return false; }
    if (!writeReg(REG_CTRL7, enable_gyro ? CTRL7_ACCEL_GYRO : CTRL7_ACCEL_ONLY)) {
        Serial.println("[ACCEL] CTRL7 write failed"); return false;
    }

    g_initialized = true;
    Serial.printf("[ACCEL] QMI8658 initialized at 0x%02X (accel 4G/250Hz%s)\n",
                  g_addr, enable_gyro ? ", gyro 256dps/250Hz" : ", gyro OFF");
    return true;
}

bool reset() {
    Serial.println("[ACCEL] Powering down and re-initializing...");
    if (g_dev) writeReg(REG_CTRL7, CTRL7_POWER_DOWN);
    delay(10);
    bool ok = begin(g_gyro_on);
    Serial.printf("[ACCEL] Re-init %s\n", ok ? "successful" : "FAILED");
    return ok;
}

bool read(AccelData& out) {
    if (!g_enabled || !g_initialized || !g_dev) return false;

    // 0x35-0x40 are contiguous (AX..AZ then GX..GZ), so accel+gyro is one 12-byte
    // burst rather than two transactions -- about +135 us on a 400 kHz bus.
    const size_t len = g_gyro_on ? 12 : 6;
    uint8_t data[12];
    if (!i2c_manager::read(*g_dev, REG_AX_L, data, len)) return false;

    out.ax_raw = (int16_t)(data[1] << 8 | data[0]);
    out.ay_raw = (int16_t)(data[3] << 8 | data[2]);
    out.az_raw = (int16_t)(data[5] << 8 | data[4]);

    out.ax_g = out.ax_raw / ACCEL_LSB_PER_G;
    out.ay_g = out.ay_raw / ACCEL_LSB_PER_G;
    out.az_g = out.az_raw / ACCEL_LSB_PER_G;

    if (g_gyro_on) {
        out.gx_raw = (int16_t)(data[7]  << 8 | data[6]);
        out.gy_raw = (int16_t)(data[9]  << 8 | data[8]);
        out.gz_raw = (int16_t)(data[11] << 8 | data[10]);
        out.gx_dps = out.gx_raw / GYRO_LSB_PER_DPS;
        out.gy_dps = out.gy_raw / GYRO_LSB_PER_DPS;
        out.gz_dps = out.gz_raw / GYRO_LSB_PER_DPS;
        out.gyro_valid = true;
    } else {
        out.gx_raw = out.gy_raw = out.gz_raw = 0;
        out.gx_dps = out.gy_dps = out.gz_dps = 0.0f;
        out.gyro_valid = false;
    }

    out.valid = true;
    out.last_update_ms = millis();
    return true;
}

void setEnabled(bool enabled) {
    g_enabled = enabled;
    Serial.printf("[ACCEL] Reads %s\n", enabled ? "ENABLED" : "DISABLED (kill switch)");
}

bool isEnabled() { return g_enabled; }

bool setGyroEnabled(bool enabled) {
    if (enabled == g_gyro_on && g_initialized) return true;
    return begin(enabled);
}

bool isGyroEnabled()  { return g_gyro_on; }
bool isInitialized()  { return g_initialized; }
uint8_t address()     { return g_addr; }

} // namespace accel_qmi8658
