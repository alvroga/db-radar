#pragma once
#include "core/arduino_compat.h"

struct CompassData {
    int16_t x_raw = 0, y_raw = 0, z_raw = 0;

    // Hard-iron-corrected vector (raw minus the stored offsets). A flat 360 spin cannot calibrate
    // Z at all -- the axis never changes what it points at, so min == max and the offset is
    // unrecoverable -- which is why the calibration overlay's tumble/figure-8 step (WP-5,
    // docs/compass_calibration_foundation.md 3.4/12) exists specifically to set cal_z_offset.
    // cz is a real hard-iron-corrected value once that step has run. It is still unused by the
    // heading formula below (2-axis atan2, unchanged) -- consuming it is Level 3 (WP-6, tilt
    // compensation), which also needs the accelerometer.
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

    // Which physical compass chip is actually present on this board. Decided once by
    // device_manager::initCompass() from the pinned GPS module choice (BH-880 ->
    // QMC5883L, BN-880 -> HMC5883L, BE-881 -> QMC5883P, LC76G -> compass skipped
    // entirely) -- never runtime-probed. See
    // docs/adr/0032-pinned-gps-module-not-always-auto-detect.md's BN-880 addendum for
    // why: the module lineup won't stay fixed at two compass-bearing types forever, so
    // only an explicit, physical choice can disambiguate reliably.
    //
    // This namespace is the single public compass entry point for the whole codebase
    // (task_manager.cpp, settings_screen.cpp, navigation.cpp, diagnostics.cpp,
    // tilt_bench.cpp) even though it now orchestrates three chips internally -- see the
    // top-of-file comment in the .cpp for why it wasn't renamed. Calibration storage,
    // health classification, and the heading formula below are chip-agnostic and stay
    // exactly as they were; only begin()/read() know which chip they're actually talking to.
    enum class ChipType : uint8_t { QMC5883L = 0, HMC5883L = 1, QMC5883P = 2 };

    bool begin(ChipType chip = ChipType::QMC5883L);  // Init the given chip; see compass_hmc5883l.h for the HMC5883L register differences
    bool reset();                    // Soft-reset chip then re-run begin() for whichever ChipType was last passed to begin() (recovers from I2C bus collisions)
    bool read(CompassData& out);     // Read XYZ, compute heading, check status
    bool isReady();                  // Check data-ready bit in STATUS register

    // Which chip begin() was last told to talk to. For callers outside this dispatch
    // (diagnostics.cpp's 'compass status'/'compass init') that need to act differently
    // per chip instead of going through read()/begin()'s own internal dispatch.
    ChipType activeChip();

    // LSB per microtesla for whichever chip is currently active — QMC5883L stays the
    // historical 120 (2G range, unverified against a from-scratch datasheet re-check but
    // unchanged from before this function existed); HMC5883L/QMC5883P values come from
    // their own headers' LSB_PER_UT constants. h_mag itself is unaffected by chip choice
    // (heading is scale-independent) — this only matters for printed uT/H0 figures.
    float lsbPerUt();

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
        DISTURBANCE,   // Sensor overflow ONLY (hardware-reported saturation). There is deliberately
                       // no magnitude-based low-ratio guess here -- an earlier version tried one and
                       // it was unreliable in the field (2026-08-02): a nearby ferromagnetic object
                       // likely INFLATES h_mag, the same direction as tilt, not the opposite, so a
                       // low-side threshold was probably backwards for the common case. See the
                       // implementation comment in classifyHealth().
    };

    const char* healthToString(CompassHealth health);

    // Smooths h_mag with a short EMA (tau ~1s) and applies hysteresis around the transition ratios
    // so the classification doesn't flicker at the noise floor (~3% relative, wp3_results.md) --
    // same pattern as the beacon proximity zone classifier. Call once per compass reading; EMA/state
    // persist internally (there is exactly one compass on this board).
    CompassHealth classifyHealth(const CompassData& data, float h0);

}
