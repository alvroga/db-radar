#include "navigation/tilt_compensation.h"
#include <math.h>

namespace tilt_compensation {

namespace {

// docs/compass_calibration_foundation.md §8.4 measured the shake spectrum (peaks
// ~2-4Hz, ~40% energy above 5Hz) but stopped short of fitting tau. 1.0s was the
// starting point; field test 2026-08-02 (hand-held nose-up tilt) showed a real, fast-
// recovering ~30deg transient heading bounce during the transition itself, caused by
// this EMA lagging the near-instantaneous mag reading while gravity is still
// converging on the new orientation -- an inherent accel-only tilt-compensation
// limitation (no gyro to track rotation rate during the motion, see WP-2's gyro-
// deferred decision), not a bug. Lowered to 0.5s to trade toward tighter transition
// response; if walking heading gets noisier as a result, that's the other side of the
// same tradeoff -- see file header of tilt_compensation.h.
constexpr float GRAVITY_EMA_TAU_S = 0.5f;

// Reference "forward" axis in accel/device frame (see header: device Y, the physical
// top edge of the screen). The east/north basis built from it is degenerate when this
// axis is parallel to gravity (device pointed straight up/down). |cross(down, fwd)|
// = sin(angle between them), so 0.05 rad-equivalent margin is roughly a 3 deg cone
// around the singularity -- generous given normal handheld tilt rarely approaches
// pitch = +/-90 deg.
constexpr float SINGULARITY_THRESHOLD = 0.05f;

bool g_gravity_valid = false;
float g_gx = 0.0f, g_gy = 0.0f, g_gz = -1.0f;
uint32_t g_last_update_ms = 0;

bool g_enabled = true;
int8_t g_sign = 1;

} // namespace

void updateGravity(float ax_g, float ay_g, float az_g, uint32_t now_ms) {
    if (!g_gravity_valid) {
        g_gx = ax_g; g_gy = ay_g; g_gz = az_g;
        g_gravity_valid = true;
        g_last_update_ms = now_ms;
        return;
    }

    float dt_s = (now_ms - g_last_update_ms) / 1000.0f;
    g_last_update_ms = now_ms;
    if (dt_s <= 0.0f || dt_s > 2.0f) {
        // Stale/first-after-gap sample -- snap instead of smoothing across the gap
        // (same guard as the compass heading EMA and the beacon RSSI EMAs use).
        g_gx = ax_g; g_gy = ay_g; g_gz = az_g;
        return;
    }

    float alpha = 1.0f - expf(-dt_s / GRAVITY_EMA_TAU_S);
    g_gx += alpha * (ax_g - g_gx);
    g_gy += alpha * (ay_g - g_gy);
    g_gz += alpha * (az_g - g_gz);
}

bool isGravityValid() { return g_gravity_valid; }

Result computeHeading(int16_t cx, int16_t cy, int16_t cz) {
    Result r;
    if (!g_enabled || !g_gravity_valid) return r;

    float gmag = sqrtf(g_gx * g_gx + g_gy * g_gy + g_gz * g_gz);
    if (gmag < 0.1f) return r;   // shouldn't happen, guards a div-by-zero below
    float dx = g_gx / gmag, dy = g_gy / gmag, dz = g_gz / gmag;

    // fwd = device Y axis (0,1,0); e = normalize(down x fwd)
    float ex = dy * 0.0f - dz * 1.0f;   //  dy*fz - dz*fy
    float ey = dz * 0.0f - dx * 0.0f;   //  dz*fx - dx*fz
    float ez = dx * 1.0f - dy * 0.0f;   //  dx*fy - dy*fx
    float emag = sqrtf(ex * ex + ey * ey + ez * ez);
    if (emag < SINGULARITY_THRESHOLD) return r;   // device pointed ~straight up/down
    ex /= emag; ey /= emag; ez /= emag;

    // n = e x down
    float nx = ey * dz - ez * dy;
    float ny = ez * dx - ex * dz;
    float nz = ex * dy - ey * dx;

    // Bench-derived signed permutation (see header): mag Y->device X, mag X->device Y,
    // mag Z->device Z.
    float mx = (float)g_sign * (float)cy;
    float my = (float)g_sign * (float)cx;
    float mz = (float)g_sign * (float)cz;

    float dot_e = mx * ex + my * ey + mz * ez;
    float dot_n = mx * nx + my * ny + mz * nz;

    float heading = atan2f(dot_e, dot_n) * 180.0f / (float)M_PI;
    if (heading < 0.0f) heading += 360.0f;

    r.heading_deg = heading;
    r.valid = true;
    return r;
}

void setEnabled(bool enabled) { g_enabled = enabled; }
bool isEnabled() { return g_enabled; }
void setSign(int8_t sign) { g_sign = (sign < 0) ? -1 : 1; }
int8_t getSign() { return g_sign; }

} // namespace tilt_compensation
