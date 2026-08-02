#pragma once
#include "core/arduino_compat.h"

struct CompassData {
    int16_t x_raw = 0, y_raw = 0, z_raw = 0;

    // Hard-iron-corrected vector (raw minus the stored offsets). cz is only
    // meaningful once a 3-axis calibration exists -- a flat 360 spin cannot
    // calibrate Z at all (the axis never changes what it points at), so
    // cal_z_offset is 0 today and cz == z_raw. See
    // docs/compass_calibration_foundation.md 3.4.
    int16_t cx = 0, cy = 0, cz = 0;

    // Horizontal field magnitude, sqrt(cx^2 + cy^2), in raw LSB (120 LSB/uT at
    // the 2 G range). Held flat with a good calibration this is CONSTANT
    // regardless of heading -- so departures from that baseline are what
    // distinguish tilt from a stale calibration from a magnetic disturbance.
    // Computed implicitly by the heading atan2 and previously discarded.
    float h_mag = 0.0f;

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

    // Level 1 health classification (docs/compass_calibration_foundation.md §5). h_mag is constant
    // vs. heading when the device is flat and calibrated; every failure mode perturbs it
    // distinguishably from a baseline H0 captured at calibration time. This DETECTS -- it does not
    // correct. Recovering true heading from a tilted reading needs the tilt axis, which a
    // magnetometer alone can't supply (that's Level 3, needs the accelerometer).
    enum class CompassHealth {
        UNCALIBRATED,  // h0 <= 0 -- no baseline to compare against
        HEALTHY,       // h_mag within tolerance of H0
        TILTED,        // h_mag elevated well above H0 -- field-confirmed: tilt INFLATES h_mag
                       // (+23% at ~45-50 deg tilt, docs/calibration/wp3_results.md), never reduces it
        DISTURBANCE,   // h_mag depressed well below H0, or sensor overflow -- a local magnetic
                       // source, not tilt
    };

    const char* healthToString(CompassHealth health);

    // Smooths h_mag with a short EMA (tau ~1s) and applies hysteresis around the transition ratios
    // so the classification doesn't flicker at the noise floor (~3% relative, wp3_results.md) --
    // same pattern as the beacon proximity zone classifier. Call once per compass reading; EMA/state
    // persist internally (there is exactly one compass on this board).
    CompassHealth classifyHealth(const CompassData& data, float h0);

}
