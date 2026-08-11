// This file is the single public compass entry point for the whole codebase (see the
// ChipType comment in compass_qmc5883l.h) -- it now orchestrates either a QMC5883L
// (BH-880) or an HMC5883L (BN-880, compass_hmc5883l.cpp) depending on the pinned GPS
// module, even though the filename/namespace still says "qmc5883l". Kept unrenamed
// deliberately: every other caller (task_manager.cpp, settings_screen.cpp,
// navigation.cpp, diagnostics.cpp, tilt_bench.cpp) already calls compass_qmc5883l::,
// and a rename would touch all of them for no functional gain -- see the "internal
// dispatch, not a public rename" ADR. Calibration, health classification, and the
// heading formula below are chip-agnostic and unaffected by which chip is active.
#include "compass_qmc5883l.h"
#include "compass_hmc5883l.h"
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

    // Which chip begin() was last told to talk to -- remembered so reset() (called from
    // task_manager.cpp's I2C recovery paths with no arguments) knows what to re-init as.
    compass_qmc5883l::ChipType active_chip = compass_qmc5883l::ChipType::QMC5883L;

    // Level 1 health classification state (docs/compass_calibration_foundation.md §5).
    // Single instance -- there is exactly one compass on this board.
    float    health_h_mag_ema  = 0.0f;
    bool     health_ema_init   = false;
    uint32_t health_last_ms    = 0;
    compass_qmc5883l::CompassHealth health_state = compass_qmc5883l::CompassHealth::UNCALIBRATED;
}

namespace compass_qmc5883l {

bool begin(ChipType chip) {
    active_chip = chip;
    initialized = false;

    if (chip == ChipType::HMC5883L) {
        initialized = compass_hmc5883l::begin();
        return initialized;
    }

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
    if (active_chip == ChipType::HMC5883L) {
        // The HMC5883L has no documented soft-reset register bit (unlike the QMC5883L's
        // CONTROL2 bit7) -- re-running begin() re-applies the same config registers,
        // which is the HMC5883L datasheet's own recommended recovery.
        Serial.println("[COMPASS] Re-initializing HMC5883L...");
        initialized = false;
        bool ok = begin(ChipType::HMC5883L);
        Serial.println(ok ? "[COMPASS] Re-init successful" : "[COMPASS] Re-init failed — device may need power cycle");
        return ok;
    }

    Serial.println("[COMPASS] Sending soft reset (CONTROL2 bit7)...");
    initialized = false;
    // Writing 0x80 to CONTROL2 triggers chip soft-reset
    i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL2, 0x80);
    delay(10);  // Datasheet: allow 5ms for reset to complete
    bool ok = begin(ChipType::QMC5883L);
    if (ok) {
        Serial.println("[COMPASS] Soft reset successful, re-initialized");
    } else {
        Serial.println("[COMPASS] Soft reset failed — device may need power cycle");
    }
    return ok;
}

bool isReady() {
    if (!initialized) return false;
    if (active_chip == ChipType::HMC5883L) {
        return compass_hmc5883l::isReady();
    }
    uint8_t status = 0;
    if (!i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_STATUS, status)) {
        return false;
    }
    return (status & STATUS_DRDY) != 0;
}

bool read(CompassData& out) {
    if (!initialized) return false;

    if (active_chip == ChipType::HMC5883L) {
        if (!compass_hmc5883l::readRaw(out.x_raw, out.y_raw, out.z_raw, out.overflow)) {
            return false;
        }
    } else {
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
    }

    // From here on, chip-agnostic -- both chips have populated x_raw/y_raw/z_raw/overflow
    // above, and calibration/heading math treats them identically.

    // Apply hard-iron calibration offsets. cal_z_offset comes from the calibration overlay's
    // tumble/figure-8 step (WP-5) -- a flat 360 spin alone cannot calibrate Z, since the axis never
    // changes what it points at (min ~= max). The heading formula below stays 2-axis regardless, so
    // a real Z offset cannot perturb the heading -- consuming cz is Level 3 (WP-6, tilt
    // compensation). See docs/compass_calibration_foundation.md 3.4.
    out.cx = out.x_raw - cal_x_offset;
    out.cy = out.y_raw - cal_y_offset;
    out.cz = out.z_raw - cal_z_offset;

    // Horizontal field magnitude. Constant with heading when flat and calibrated;
    // every failure mode perturbs it distinguishably (5.1 of the doc above).
    out.h_mag = sqrtf((float)out.cx * (float)out.cx + (float)out.cy * (float)out.cy);

    // Compute heading (degrees, 0-360, magnetic north)
    float heading = atan2f((float)out.cy, (float)out.cx) * 180.0f / M_PI;
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

const char* healthToString(CompassHealth health) {
    switch (health) {
        case CompassHealth::UNCALIBRATED: return "Uncalibrated";
        case CompassHealth::HEALTHY:      return "Healthy";
        case CompassHealth::TILTED:       return "Tilted";
        case CompassHealth::DISTURBANCE:  return "Interference";
    }
    return "Unknown";
}

CompassHealth classifyHealth(const CompassData& data, float h0) {
    if (h0 <= 0.0f) {
        health_state = CompassHealth::UNCALIBRATED;
        return health_state;
    }

    if (data.overflow) {
        // Sensor-reported saturation -- a hardware fact, not a guessed threshold. This is the
        // only trigger for DISTURBANCE; see the note below on why a magnitude-based low-ratio
        // guess was removed.
        health_state = CompassHealth::DISTURBANCE;
        return health_state;
    }

    // dt-based EMA so the smoothing doesn't depend on the caller's exact call rate.
    uint32_t now = data.last_update_ms ? data.last_update_ms : millis();
    if (!health_ema_init) {
        health_h_mag_ema = data.h_mag;
        health_ema_init = true;
        health_last_ms = now;
    } else {
        float dt_s = (now - health_last_ms) / 1000.0f;
        health_last_ms = now;
        if (dt_s > 0.0f && dt_s < 5.0f) {
            constexpr float TAU_S = 1.0f;  // smooths the ~3% noise floor without much lag
            float alpha = 1.0f - expf(-dt_s / TAU_S);
            health_h_mag_ema += alpha * (data.h_mag - health_h_mag_ema);
        } else {
            // Large/negative gap (standby wake, first sample after reinit) -- a stale EMA would
            // lie, so snap to the fresh reading instead of smoothing across the gap.
            health_h_mag_ema = data.h_mag;
        }
    }

    float ratio = health_h_mag_ema / h0;

    // Hysteresis band for TILT, informed by docs/calibration/wp3_results.md: noise floor is
    // ~2.5-3.3% relative, tilt inflates h_mag by ~23% at 45-50 deg. Entry sits well above the
    // noise floor; exit sits below entry so the state doesn't chatter at the boundary -- same
    // shape as the beacon proximity zone hysteresis (+-3 dBm).
    //
    // There is deliberately NO magnitude-based low-ratio threshold for DISTURBANCE. An earlier
    // version guessed one (ratio < 0.85), reasoning a disturbance might weaken the field -- that
    // was never field-verified, and is probably backwards for the common case: a nearby
    // ferromagnetic object concentrates field lines, which INFLATES h_mag in the same direction as
    // tilt, not the opposite. Confirmed unreliable in the field 2026-08-02 (hit-or-miss near metal
    // objects, indistinguishable from a misfire). Only the hardware overflow flag above is trusted
    // for disturbance until someone logs h_mag walking past a real disturbance and derives an
    // actual threshold -- see docs/compass_calibration_foundation.md §5.1.
    constexpr float TILT_ENTER = 1.12f, TILT_EXIT = 1.08f;

    if (health_state == CompassHealth::UNCALIBRATED || health_state == CompassHealth::DISTURBANCE) {
        // Just gained a baseline, or overflow just cleared -- classify fresh rather than latching.
        health_state = (ratio > TILT_ENTER) ? CompassHealth::TILTED : CompassHealth::HEALTHY;
        return health_state;
    }

    if (ratio > TILT_ENTER) {
        health_state = CompassHealth::TILTED;
    } else if (ratio <= TILT_EXIT) {
        health_state = CompassHealth::HEALTHY;
    }
    // else: ratio sits in the hysteresis gap -- keep the current state.

    return health_state;
}

} // namespace compass_qmc5883l
