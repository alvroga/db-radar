#pragma once
#include "core/arduino_compat.h"

// Minimal accelerometer driver for the QMI8658 6-axis IMU on the Waveshare main
// board (I2C 0x6A/0x6B, already present and ACKing on the shared bus today).
//
// Purpose: supply "which way is down" so heading can eventually be tilt-compensated.
// The 2-axis magnetometer heading inverts past ~31.5 deg of tilt -- field-confirmed --
// and only gravity can fix that. See docs/compass_calibration_foundation.md 3.3, 6.
//
// This is NOT the old imu_sampling.cpp. That did *gyro* heading fusion at 100Hz and
// was removed for good reason: a gyro measures angular rate, so heading needs
// integration, so it drifts. This reads the *accelerometer*, which measures gravity
// and never drifts. Different sensor, different job. The gyro is available here for
// logging only (field sample 9), to decide whether it is worth using at all.

struct AccelData {
    // Raw counts, as read.
    int16_t ax_raw = 0, ay_raw = 0, az_raw = 0;
    int16_t gx_raw = 0, gy_raw = 0, gz_raw = 0;

    // Scaled. Held still, sqrt(ax^2+ay^2+az^2) should be ~1.0 g.
    float ax_g = 0.0f, ay_g = 0.0f, az_g = 0.0f;
    float gx_dps = 0.0f, gy_dps = 0.0f, gz_dps = 0.0f;

    bool gyro_valid = false;    // false unless the gyro was explicitly enabled
    bool valid = false;
    uint32_t last_update_ms = 0;
};

namespace accel_qmi8658 {

    // Probe 0x6A then 0x6B, verify WHO_AM_I, configure accel (and optionally gyro).
    // Returns false if the chip does not answer on either address.
    bool begin(bool enable_gyro = false);

    // Power the sensors down and re-run begin(). Mirrors compass_qmc5883l::reset()
    // so the System Task can recover it the same way after WiFi/standby.
    bool reset();

    // One burst read: 6 bytes accel-only, or 12 bytes when the gyro is enabled
    // (0x35-0x40 are contiguous, so both cost one transaction, not two).
    // Returns false when disabled, uninitialized, or the bus read fails.
    bool read(AccelData& out);

    // Runtime kill switch. Required by the I2C risk assessment (doc 7.4): the bus
    // has a tuned timing floor whose cause is undiagnosed (ADR-0013) and an open
    // freeze issue (FT-06), so adding a sixth actively-read device must be
    // instantly reversible in the field without a reflash. setEnabled(false) stops
    // all traffic to the chip; it does not power it down (that needs a bus write).
    void setEnabled(bool enabled);
    bool isEnabled();

    // Gyro is off by default -- it roughly doubles the burst and draws several
    // times the accelerometer's current, for no benefit until sample 9 says
    // otherwise. Enabling re-runs begin().
    bool setGyroEnabled(bool enabled);
    bool isGyroEnabled();

    bool isInitialized();
    uint8_t address();          // 0x6A or 0x6B, 0 if not found

}
