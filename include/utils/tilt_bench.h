#pragma once
#include "core/arduino_compat.h"

// WP-6 bench procedure: determine the fixed rotation between the magnetometer's and
// accelerometer's sensor frames. See docs/compass_tilt_bench.md and
// docs/compass_calibration_foundation.md §6.2 item 3 / §10.
//
// ⚠️ Same USB constraint as field_log, for a related but distinct reason: this
// procedure requires holding the device in NOSE-UP/DOWN and LEFT/RIGHT-EDGE-DOWN
// poses. A USB cable makes those poses awkward to hold cleanly, and a cable/charger
// is itself a plausible local magnetic-field source right next to a magnetometer --
// exactly the kind of contamination this whole calibration effort exists to avoid.
// Session data lands in a CSV under /sdcard/logs and comes off over WiFi afterward,
// same retrieval path as field_log (the /logs page already serves .csv files).
//
// Unlike field_log, this is NOT a continuous sensor stream -- it is one row per
// user-triggered pose capture (at most a handful per session), so there is no ring
// buffer or writer task. A direct fopen/fwrite/fflush from the UI Task on a button
// tap is the same class of occasional-blocking-write already accepted elsewhere in
// this codebase (e.g. the compass calibration overlay's NVS save).

namespace tilt_bench {

// The 6-pose bench protocol from docs/compass_tilt_bench.md. FLAT_REPEAT exists as
// a drift/repeatability check, not a distinct physical orientation.
enum class Pose : uint8_t {
    FLAT = 0,
    NOSE_UP,
    NOSE_DOWN,
    LEFT_EDGE_DOWN,
    RIGHT_EDGE_DOWN,
    FLAT_REPEAT,
    COUNT
};

const char* poseName(Pose p);

struct CaptureResult {
    bool  ok = false;
    float ax_g = 0.0f, ay_g = 0.0f, az_g = 0.0f;
    int16_t cx = 0, cy = 0, cz = 0;
    float heading_deg = 0.0f;
};

// True when there is somewhere to write (SD mounted, logs dir exists) -- mirrors
// field_log::storageAvailable(). Cheap: cached, not queried on every call.
bool storageAvailable();

bool isSessionActive();
uint32_t rowCount();
const char* currentFilename();     // "" when no session is open

// Opens /sdcard/logs/tiltbench_<NNN>.csv and writes the header block. NNN is
// monotonic across reboots, scanned from the directory like field_log's cal_NNN.
bool startSession();

// Reads accel + compass once (same driver paths as `compass tiltbench` / the
// existing `accel read` / `compass read` commands -- calibration already applied)
// and appends one row tagged with `pose`. No-op (ok=false) if no session is open.
CaptureResult capture(Pose pose);

// Flushes and closes the file.
void endSession();

uint16_t lastSessionNumber();

} // namespace tilt_bench
