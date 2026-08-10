#pragma once
#include "core/arduino_compat.h"

// Field data logger — one CSV file per sample, start/stop from a DEV screen.
//
// Exists because the constants the compass calibration work needs (H0, the tilt
// threshold, the accel gravity tau, the shake spectrum, the actual tilt bias while
// walking) cannot be guessed and must be measured outdoors. See
// docs/compass_calibration_foundation.md §8.
//
// ⚠️ HARD CONSTRAINT that shapes this whole module: **the serial monitor requires
// USB, and USB also powers the 5V rail and charges the battery**, so a
// battery-powered field trip produces no serial output at all. Every "walk around
// then read the log" approach is impossible. Data must land in files and be
// retrieved later over WiFi (the /logs page already serves and downloads them).
//
// ⚠️ STORAGE IS THE PHYSICAL SD CARD, not internal flash. The plan document
// asserted /sdcard was the FFat mount; it is not — device_manager::initSD() calls
// esp_vfs_fat_sdmmc_mount("/sdcard", ...) against the SDMMC host, and the 11.7MB
// `ffat` partition in partitions_ota.csv is never mounted by anything. **A card
// must be inserted or there is nowhere to write**, and capacity is the card's.

namespace field_log {

// The fixed sample vocabulary from doc §8.3. Baked into the filename so a sample
// is identified at capture time rather than reconstructed from memory afterwards.
enum class Label : uint8_t {
    FLAT360 = 0,      // still, flat, slow 360 — establishes H0 and the hard-iron ripple
    PHONE360,         // still, phone-style, slow 360 — the "not flat" threshold
    WALK_STRAIGHT,    // ~100m straight — compass vs GPS course = the real tilt bias
    STAND_STILL,      // 60s still and flat — the noise floor
    DISTURBANCE,      // past a car / fence / utility box — what interference looks like
    FREEFORM,         // tumble through all orientations — 3-axis calibration feasibility
    SHAKE_100HZ,      // 30s walking at 100Hz with the gyro on — the shake spectrum
    COUNT
};

const char* labelName(Label l);

// One sensor tick. Filled by the System Task and pushed into a RAM ring; the
// writer task formats and writes it. Kept as raw-ish values so no interpretation
// is baked in at capture time — analysis happens offline on a computer.
struct Row {
    uint32_t ms;                        // millis() at capture

    int16_t  mx, my, mz;                // magnetometer raw
    int16_t  cx, cy, cz;                // magnetometer hard-iron corrected
    float    h_mag;                     // sqrt(cx^2+cy^2), the health metric
    float    heading_mag;               // degrees, before declination
    float    heading_true;              // degrees, after declination

    int16_t  ax, ay, az;                // accelerometer raw counts
    int16_t  gx, gy, gz;                // gyro raw counts (0 unless gyro enabled)

    double   lat, lon;
    float    speed_kn, course, hdop;
    uint8_t  sats;
    uint8_t  zoom;

    // bit0 gps_fix_valid, bit1 mag_overflow, bit2 accel_valid, bit3 gyro_valid,
    // bit4 compass_valid
    uint8_t  flags;
};

enum Flag : uint8_t {
    FLAG_GPS_FIX    = 1 << 0,
    FLAG_MAG_OVF    = 1 << 1,
    FLAG_ACCEL_OK   = 1 << 2,
    FLAG_GYRO_OK    = 1 << 3,
    FLAG_COMPASS_OK = 1 << 4,
};

struct Stats {
    bool     recording      = false;
    Label    label          = Label::FLAT360;
    uint32_t elapsed_ms     = 0;
    uint32_t rows_queued    = 0;   // pushed by the sensor tick
    uint32_t rows_written   = 0;   // actually made it to the file
    uint32_t rows_dropped   = 0;   // ring was full — writer could not keep up
    uint32_t file_bytes     = 0;
    uint64_t free_bytes     = 0;
    char     filename[64]   = {};
};

// Allocate the PSRAM ring and start the writer task. Safe to call when no SD card
// is present — startSample() will then fail with a clear reason.
bool begin();

// Stop any in-progress sample (flushed and closed, same as stopSample()), then
// tear down the writer task and free the PSRAM ring/mutex. Blocks briefly while
// the writer task finishes its current cycle and deletes itself. Safe to call
// when begin() was never called. Call begin() again to resume logging.
void end();

bool startSample(Label label);
void stopSample();
bool isRecording();
Label currentLabel();

// Push one row. **Called from the System Task's sensor tick — must never block.**
// Writes into a lock-free single-producer ring; if the ring is full the row is
// dropped and counted rather than waiting on the filesystem. Silently ignored when
// not recording.
void appendRow(const Row& r);

Stats stats();

// True when there is somewhere to write (SD mounted and the logs dir exists).
bool storageAvailable();

// Highest sample number seen in the logs directory. Filenames are
// cal_<NNN>_<label>.csv with NNN monotonic, so samples sort in capture order.
uint16_t lastSampleNumber();

} // namespace field_log
