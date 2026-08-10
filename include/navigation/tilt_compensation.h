#pragma once
#include "core/arduino_compat.h"

// WP-6, Level 3 tilt compensation (docs/compass_calibration_foundation.md 6, WP-6).
//
// The mag<->accel frame rotation was an unknown fixed rotation between two chips on
// different PCBs -- not derivable from either datasheet (see docs/compass_tilt_bench.md).
// It was measured via the Settings > DEV > Tilt Bench procedure (tiltbench_001.csv,
// 2026-08-02, 2 passes x 6 poses) and fit as a signed permutation:
//
//     device_x = cy   device_y = cx   device_z = cz
//
// (mag Y -> device X, mag X -> device Y, mag Z -> device Z, no extra negation). This
// was NOT picked from a first-principles accelerometer-convention argument -- it is
// the one candidate, out of the sign x permutation search, whose resulting heading (a)
// stays self-consistent across all 12 bench rows once the two poses that put the
// reference axis parallel to gravity are excluded (nose-up/down -- see below), and (b)
// reduces EXACTLY to the existing atan2(cy, cx) formula in the zero-tilt limit, with no
// extra offset -- i.e. it is backward-compatible with the declination calibration
// already tuned against that formula. A first-principles physics argument (standard
// accelerometer sign convention + local magnetic inclination) had favored the opposite
// global sign; the bench self-consistency check is stronger evidence because it is tied
// to this actual chip's measured convention, not an assumed textbook one -- so it wins.
//
// The reference "forward" axis below (device Y, i.e. accel Y, i.e. the physical top
// edge of the screen) is degenerate when THAT axis is parallel to gravity -- pointing
// the device straight up or straight down (pitch = +/-90 deg). This is why nose-up and
// nose-down bench rows didn't fit the same consistency check as the other four poses:
// not a sign bug, gimbal lock on the chosen reference axis. computeHeading() detects
// this and returns !valid rather than a garbage angle.
//
// Global sign confirmed 2026-08-02 via live hand-held use: heading settles back to the
// correct value through flat/nose-up/recovery, not a steady ~180 deg offset, so the
// bench-derived default (+1, no extra negation) is correct as shipped. setSign(-1) /
// `compass tilt sign` stays available as a runtime revert path regardless, per this
// project's standing rule of keeping empirically-fixed signs field-adjustable.

namespace tilt_compensation {

struct Result {
    float heading_deg = NAN;   // 0-360, same convention as the existing 2-axis heading
    bool valid = false;        // false when gravity estimate not yet seeded, or near
                                // the reference-axis singularity (device pointed
                                // straight up/down)
};

// Feed one accelerometer sample (device/accel frame, g units) into the gravity
// tau-EMA. Call once per tick the accelerometer is read, independent of whether a
// heading is computed that tick -- the EMA needs a steady sample rate to keep its
// tau meaningful (same reasoning as the beacon RSSI EMAs, see CLAUDE.md Render
// Pipeline / continuous_over_hysteresis).
void updateGravity(float ax_g, float ay_g, float az_g, uint32_t now_ms);

// True once updateGravity() has run at least once (i.e. a gravity estimate exists).
bool isGravityValid();

// Compute a tilt-compensated heading from calibrated (hard-iron corrected) mag counts
// -- compass_qmc5883l's cx/cy/cz -- using the current gravity estimate. Independent of
// declination; apply compass_declination_deg the same way the caller already does for
// the 2-axis heading.
Result computeHeading(int16_t cx, int16_t cy, int16_t cz);

// Runtime kill switch + sign flip, both session-only (not NVS-persisted) so a bad
// reading in the field can be reverted or resigned without a reflash -- same reasoning
// as accel_qmi8658::setEnabled() (I2C risk assessment, doc 7.4).
void setEnabled(bool enabled);
bool isEnabled();
void setSign(int8_t sign);   // clamps to -1 or +1
int8_t getSign();

}
