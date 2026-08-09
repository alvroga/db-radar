#include "tilt_bench.h"
#include "settings_manager.h"
#include "rtc_pcf85063.h"
#include "accel_qmi8658.h"
#include "compass_qmc5883l.h"

#include <esp_vfs_fat.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace {

constexpr const char* LOGS_DIR = "/sdcard/logs";

const char* const POSE_NAMES[] = {
    "flat", "nose-up", "nose-down", "left-edge-down", "right-edge-down", "flat-repeat"
};
static_assert(sizeof(POSE_NAMES) / sizeof(POSE_NAMES[0]) ==
              (size_t)tilt_bench::Pose::COUNT, "pose name table out of sync");

bool     g_storage_ok_cached = false;
uint16_t g_next_session_no   = 1;
FILE*    g_file              = nullptr;
char     g_filename[64]      = {};
uint32_t g_row_count          = 0;
uint32_t g_start_ms          = 0;

bool ensureLogsDir() {
    struct stat st;
    if (stat(LOGS_DIR, &st) == 0) return true;
    return mkdir(LOGS_DIR, 0777) == 0;
}

// Scan for the highest tiltbench_<NNN> already present so numbering survives a
// reboot between sessions, mirroring field_log's scanHighestSampleNo().
uint16_t scanHighestSessionNo() {
    DIR* dir = opendir(LOGS_DIR);
    if (!dir) return 0;
    uint16_t highest = 0;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        unsigned n = 0;
        if (sscanf(e->d_name, "tiltbench_%u", &n) == 1 && n > highest && n < 10000) {
            highest = (uint16_t)n;
        }
    }
    closedir(dir);
    return highest;
}

void writeHeader(FILE* f) {
    const auto& s = settings_manager::getSettings();

    rtc::Time t{};
    bool have_rtc = rtc::read(t) && t.valid;

    fprintf(f, "# db-radar tilt bench (WP-6, docs/compass_tilt_bench.md)\n");
    fprintf(f, "# firmware=%s build=%lu\n", FW_VERSION, (unsigned long)FW_BUILD_TS);
    if (have_rtc) {
        fprintf(f, "# rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
                t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else {
        fprintf(f, "# rtc=unavailable\n");
    }
    fprintf(f, "# compass_cal_x=%d compass_cal_y=%d compass_cal_z=%d calibrated=%d\n",
            s.compass_cal_x, s.compass_cal_y, s.compass_cal_z, s.compass_calibrated ? 1 : 0);
    fprintf(f, "# mag_scale_lsb_per_uT=120 accel_lsb_per_g=8192\n");
    fprintf(f, "pose,ms,ax_g,ay_g,az_g,cx,cy,cz,heading_deg\n");
}

} // namespace

namespace tilt_bench {

const char* poseName(Pose p) {
    size_t i = (size_t)p;
    return i < (size_t)Pose::COUNT ? POSE_NAMES[i] : "?";
}

bool storageAvailable() {
    struct stat sb;
    g_storage_ok_cached = ensureLogsDir() && (stat(LOGS_DIR, &sb) == 0);
    return g_storage_ok_cached;
}

bool isSessionActive() { return g_file != nullptr; }
uint32_t rowCount()    { return g_row_count; }

const char* currentFilename() {
    if (!g_file) return "";
    const char* slash = strrchr(g_filename, '/');
    return slash ? slash + 1 : g_filename;
}

uint16_t lastSessionNumber() { return (uint16_t)(g_next_session_no - 1); }

bool startSession() {
    if (g_file) return true;   // already open

    if (!storageAvailable()) {
        Serial.println("[TILTBENCH] No SD card / logs dir -- nowhere to write");
        return false;
    }

    g_next_session_no = scanHighestSessionNo() + 1;
    snprintf(g_filename, sizeof(g_filename), "%s/tiltbench_%03u.csv",
             LOGS_DIR, (unsigned)g_next_session_no);

    g_file = fopen(g_filename, "wb");
    if (!g_file) {
        Serial.printf("[TILTBENCH] Failed to open %s\n", g_filename);
        return false;
    }

    writeHeader(g_file);
    fflush(g_file);
    g_row_count = 0;
    g_start_ms = millis();
    Serial.printf("[TILTBENCH] Session -> %s\n", g_filename);
    return true;
}

CaptureResult capture(Pose pose) {
    CaptureResult r;

    AccelData ad;
    bool accel_ok = accel_qmi8658::read(ad);
    CompassData cd;
    bool mag_ok = compass_qmc5883l::read(cd);

    if (!accel_ok || !mag_ok) {
        Serial.printf("[TILTBENCH] Capture failed: accel=%s mag=%s\n",
                      accel_ok ? "ok" : "FAIL", mag_ok ? "ok" : "FAIL");
        return r;
    }

    r.ok = true;
    r.ax_g = ad.ax_g; r.ay_g = ad.ay_g; r.az_g = ad.az_g;
    r.cx = cd.cx; r.cy = cd.cy; r.cz = cd.cz;
    r.heading_deg = cd.heading;

    if (g_file) {
        fprintf(g_file, "%s,%lu,%.4f,%.4f,%.4f,%d,%d,%d,%.1f\n",
                poseName(pose), (unsigned long)(millis() - g_start_ms),
                r.ax_g, r.ay_g, r.az_g, r.cx, r.cy, r.cz, r.heading_deg);
        fflush(g_file);
        g_row_count++;
    }

    return r;
}

void endSession() {
    if (!g_file) return;
    fflush(g_file);
    fclose(g_file);
    g_file = nullptr;
    Serial.printf("[TILTBENCH] Session closed: %lu rows -> %s\n",
                  (unsigned long)g_row_count, g_filename);
}

} // namespace tilt_bench
