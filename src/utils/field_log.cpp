#include "field_log.h"
#include "settings_manager.h"
#include "rtc_pcf85063.h"
#include "accel_qmi8658.h"
#include "i2c_manager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char* LOGS_DIR = "/sdcard/logs";

// Ring capacity. At 10Hz this is ~50s of slack against a writer that drains every
// 250ms — absurdly generous, and deliberately so: the 100Hz mode (sample 9) burns
// it 10x faster, and an SD write that stalls for a second must not cost rows.
constexpr size_t RING_ROWS = 512;

// Auto-stop guards, so a session forgotten in a pocket cannot fill the card.
constexpr uint32_t MAX_SAMPLE_MS  = 10u * 60u * 1000u;   // 10 minutes
constexpr uint32_t MAX_FILE_BYTES = 8u * 1024u * 1024u;  // 8 MB
constexpr uint64_t MIN_FREE_BYTES = 2u * 1024u * 1024u;  // refuse to start below this

constexpr uint32_t WRITER_PERIOD_MS = 250;

// --- ring (single producer: System Task; single consumer: writer task) --------
// head/tail are 32-bit aligned, so loads and stores are atomic on this core; with
// exactly one producer and one consumer that is sufficient without a lock. A lock
// here would put filesystem-shaped latency on the sensor clock, which is the one
// thing this module must not do.
field_log::Row* g_ring = nullptr;
volatile uint32_t g_head = 0;   // next write index (producer)
volatile uint32_t g_tail = 0;   // next read index (consumer)

SemaphoreHandle_t g_ctrl_mutex = nullptr;   // guards the control fields below only
TaskHandle_t      g_writer_task = nullptr;

enum class State : uint8_t { IDLE, START_REQUESTED, RECORDING, STOP_REQUESTED };
volatile State g_state = State::IDLE;

field_log::Label g_label = field_log::Label::FLAT360;
char     g_filename[64]   = {};
uint32_t g_start_ms       = 0;
uint32_t g_rows_queued    = 0;
uint32_t g_rows_written   = 0;
uint32_t g_rows_dropped   = 0;
uint32_t g_file_bytes     = 0;
uint16_t g_next_sample_no = 1;
bool     g_begun          = false;

const char* const LABEL_NAMES[] = {
    "flat360", "phone360", "walk-straight", "stand-still",
    "disturbance", "freeform", "shake-100hz"
};
static_assert(sizeof(LABEL_NAMES) / sizeof(LABEL_NAMES[0]) ==
              (size_t)field_log::Label::COUNT, "label name table out of sync");

// Free space is CACHED, not queried on demand.
//
// esp_vfs_fat_info() calls f_getfree(), which on FAT32 can walk the entire FAT to
// count free clusters the first time it runs. stats() is called from the UI Task
// every 500ms by the live readout, so querying there would put an unbounded
// filesystem scan on the task that owns LVGL. The writer task (Core 0, priority 1,
// no deadline) refreshes it instead; everyone else reads the cached value.
volatile uint64_t g_free_bytes_cached = 0;
volatile bool     g_storage_ok_cached  = false;

uint64_t queryFreeBytes() {
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &freeb) != ESP_OK) return 0;
    return freeb;
}

bool ensureLogsDir() {
    struct stat st;
    if (stat(LOGS_DIR, &st) == 0) return true;
    return mkdir(LOGS_DIR, 0777) == 0;
}

// Scan for the highest cal_<NNN>_ already present so numbering keeps increasing
// across reboots — the trip spans several power cycles and samples must stay in
// capture order.
uint16_t scanHighestSampleNo() {
    DIR* dir = opendir(LOGS_DIR);
    if (!dir) return 0;
    uint16_t highest = 0;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        unsigned n = 0;
        if (sscanf(e->d_name, "cal_%u_", &n) == 1 && n > highest && n < 10000) {
            highest = (uint16_t)n;
        }
    }
    closedir(dir);
    return highest;
}

// Header block. One per file, so a sample is self-describing when opened months
// later with no memory of the conditions it was taken under.
void writeHeader(FILE* f) {
    const auto& s = settings_manager::getSettings();

    rtc::Time t{};
    bool have_rtc = rtc::read(t) && t.valid;

    fprintf(f, "# cc-radar field sample\n");
    fprintf(f, "# firmware=%s build=%lu\n", FW_VERSION, (unsigned long)FW_BUILD_TS);
    if (have_rtc) {
        fprintf(f, "# rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
                t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else {
        fprintf(f, "# rtc=unavailable\n");
    }
    fprintf(f, "# label=%s\n", field_log::labelName(g_label));
    fprintf(f, "# uptime_ms=%lu\n", (unsigned long)millis());
    fprintf(f, "# compass_cal_x=%d compass_cal_y=%d compass_cal_z=%d calibrated=%d\n",
            s.compass_cal_x, s.compass_cal_y, s.compass_cal_z, s.compass_calibrated ? 1 : 0);
    fprintf(f, "# declination_deg=%.3f declination_valid=%d\n",
            s.compass_declination_deg, s.compass_declination_valid ? 1 : 0);
    // H0 is not captured yet — that is WP-4, and it needs this data first.
    fprintf(f, "# h0=unknown\n");
    fprintf(f, "# zoom=%u\n", (unsigned)s.default_zoom_level);
    fprintf(f, "# accel_enabled=%d accel_addr=0x%02X gyro_enabled=%d\n",
            accel_qmi8658::isEnabled() ? 1 : 0, accel_qmi8658::address(),
            accel_qmi8658::isGyroEnabled() ? 1 : 0);
    fprintf(f, "# mag_scale_lsb_per_uT=120 accel_lsb_per_g=8192 gyro_lsb_per_dps=128\n");
    // Sampling rate is nominal — always trust the ms column over this.
    fprintf(f, "# nominal_rate_hz=%d\n",
            g_label == field_log::Label::SHAKE_100HZ ? 100 : 10);
    // I2C health at both ends of the sample. The 100Hz mode (sample 9) deliberately
    // accepts a bus risk the rest of the firmware avoids — ADR-0013's timing floor
    // is undiagnosed and FT-06 is open — so the evidence for whether it cost
    // anything has to be inside the file, not in a serial log nobody can read on
    // battery. Any rise in failures across a sample invalidates that sample.
    const auto& st0 = i2c_manager::getStats();
    fprintf(f, "# i2c_start_ops=%lu i2c_start_fails=%lu\n",
            (unsigned long)st0.total_ops, (unsigned long)st0.failed_ops);
    fprintf(f, "ms,mx,my,mz,cx,cy,cz,h_mag,heading_mag,heading_true,"
               "ax,ay,az,gx,gy,gz,"
               "lat,lon,speed_kn,course,hdop,sats,fix_valid,zoom,mag_overflow,"
               "accel_valid,gyro_valid\n");
}

void writeFooter(FILE* f) {
    fprintf(f, "# rows_queued=%lu rows_written=%lu rows_dropped=%lu\n",
            (unsigned long)g_rows_queued, (unsigned long)g_rows_written,
            (unsigned long)g_rows_dropped);
    fprintf(f, "# duration_ms=%lu\n", (unsigned long)(millis() - g_start_ms));
    const auto& st1 = i2c_manager::getStats();
    fprintf(f, "# i2c_end_ops=%lu i2c_end_fails=%lu\n",
            (unsigned long)st1.total_ops, (unsigned long)st1.failed_ops);
}

int formatRow(char* buf, size_t cap, const field_log::Row& r) {
    return snprintf(buf, cap,
        "%lu,%d,%d,%d,%d,%d,%d,%.1f,%.2f,%.2f,"
        "%d,%d,%d,%d,%d,%d,"
        "%.7f,%.7f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u,%u\n",
        (unsigned long)r.ms,
        r.mx, r.my, r.mz, r.cx, r.cy, r.cz,
        r.h_mag, r.heading_mag, r.heading_true,
        r.ax, r.ay, r.az, r.gx, r.gy, r.gz,
        r.lat, r.lon, r.speed_kn, r.course, r.hdop,
        (unsigned)r.sats,
        (unsigned)((r.flags & field_log::FLAG_GPS_FIX)    ? 1 : 0),
        (unsigned)r.zoom,
        (unsigned)((r.flags & field_log::FLAG_MAG_OVF)    ? 1 : 0),
        (unsigned)((r.flags & field_log::FLAG_ACCEL_OK)   ? 1 : 0),
        (unsigned)((r.flags & field_log::FLAG_GYRO_OK)    ? 1 : 0));
}

void writerTask(void*) {
    FILE* f = nullptr;
    char line[256];

    for (;;) {
        State st = g_state;

        if (st == State::START_REQUESTED) {
            g_head = g_tail = 0;
            g_rows_queued = g_rows_written = g_rows_dropped = 0;
            g_file_bytes = 0;
            f = fopen(g_filename, "wb");
            if (!f) {
                Serial.printf("[FLOG] Failed to open %s\n", g_filename);
                g_state = State::IDLE;
            } else {
                writeHeader(f);
                fflush(f);
                g_start_ms = millis();
                g_state = State::RECORDING;
                Serial.printf("[FLOG] Recording -> %s\n", g_filename);
            }
        }

        // Drain whatever is queued, whether recording or stopping.
        if (f) {
            uint32_t tail = g_tail;
            while (tail != g_head) {
                int n = formatRow(line, sizeof(line), g_ring[tail % RING_ROWS]);
                if (n > 0) {
                    fwrite(line, 1, (size_t)n, f);
                    g_file_bytes += (uint32_t)n;
                    g_rows_written++;
                }
                tail++;
                g_tail = tail;
            }
            fflush(f);
        }

        st = g_state;

        // Auto-stop guards. Checked here rather than on the sensor path so the
        // sensor tick stays a pure ring push.
        if (st == State::RECORDING && f) {
            if (millis() - g_start_ms >= MAX_SAMPLE_MS) {
                Serial.println("[FLOG] Auto-stop: time cap reached");
                g_state = State::STOP_REQUESTED;
            } else if (g_file_bytes >= MAX_FILE_BYTES) {
                Serial.println("[FLOG] Auto-stop: size cap reached");
                g_state = State::STOP_REQUESTED;
            }
        }

        if (g_state == State::STOP_REQUESTED) {
            if (f) {
                writeFooter(f);
                fclose(f);
                f = nullptr;
                Serial.printf("[FLOG] Stopped: %s (%lu rows, %lu bytes, %lu dropped)\n",
                              g_filename, (unsigned long)g_rows_written,
                              (unsigned long)g_file_bytes, (unsigned long)g_rows_dropped);
            }
            g_state = State::IDLE;
        }

        // Refresh the cached free-space figure here rather than in stats(), so the
        // potentially-slow FAT scan never lands on the UI Task.
        static uint32_t last_free_check = 0;
        if (millis() - last_free_check >= 5000) {
            last_free_check = millis();
            g_free_bytes_cached = queryFreeBytes();
            struct stat sb;
            g_storage_ok_cached = (stat(LOGS_DIR, &sb) == 0);
        }

        vTaskDelay(pdMS_TO_TICKS(WRITER_PERIOD_MS));
    }
}

} // namespace

namespace field_log {

const char* labelName(Label l) {
    uint8_t i = (uint8_t)l;
    return (i < (uint8_t)Label::COUNT) ? LABEL_NAMES[i] : "unknown";
}

bool begin() {
    if (g_begun) return true;

    g_ring = (Row*)heap_caps_calloc(RING_ROWS, sizeof(Row), MALLOC_CAP_SPIRAM);
    if (!g_ring) {
        Serial.println("[FLOG] PSRAM ring allocation failed — field logging unavailable");
        return false;
    }

    g_ctrl_mutex = xSemaphoreCreateMutex();
    if (!g_ctrl_mutex) {
        Serial.println("[FLOG] Mutex creation failed");
        heap_caps_free(g_ring);
        g_ring = nullptr;
        return false;
    }

    if (ensureLogsDir()) {
        g_next_sample_no = (uint16_t)(scanHighestSampleNo() + 1);
    }
    // One blocking query at boot; the writer task keeps both fresh from then on.
    g_free_bytes_cached = queryFreeBytes();
    {
        struct stat sb;
        g_storage_ok_cached = (stat(LOGS_DIR, &sb) == 0);
    }

    // Core 0, priority 1 — the lowest application priority, alongside the Network
    // and System tasks. It must never compete with the UI Task on Core 1, and its
    // work is bulk filesystem I/O with no deadline.
    BaseType_t ok = xTaskCreatePinnedToCore(
        writerTask, "FieldLogW", 6144, nullptr, 1, &g_writer_task, 0);
    if (ok != pdPASS) {
        Serial.println("[FLOG] Writer task creation failed");
        heap_caps_free(g_ring);
        g_ring = nullptr;
        return false;
    }

    g_begun = true;
    Serial.printf("[FLOG] Ready — ring %u rows (%u KB PSRAM), next sample #%u, %llu MB free\n",
                  (unsigned)RING_ROWS, (unsigned)(RING_ROWS * sizeof(Row) / 1024),
                  (unsigned)g_next_sample_no, (unsigned long long)(g_free_bytes_cached / (1024 * 1024)));
    return true;
}

// Cached, for the same reason as free space: the live readout polls this from the
// UI Task, and on a card-less device the uncached version would retry mkdir()
// every 500ms forever.
bool storageAvailable() { return g_storage_ok_cached; }

uint16_t lastSampleNumber() { return (uint16_t)(g_next_sample_no - 1); }

bool startSample(Label label) {
    if (!g_begun) {
        Serial.println("[FLOG] begin() not called");
        return false;
    }
    if (g_state != State::IDLE) {
        Serial.println("[FLOG] Already recording");
        return false;
    }
    if (!storageAvailable()) {
        Serial.println("[FLOG] No SD card / logs dir — cannot record");
        return false;
    }
    uint64_t freeb = g_free_bytes_cached;
    if (freeb < MIN_FREE_BYTES) {
        Serial.printf("[FLOG] Only %llu KB free — refusing to start\n",
                      (unsigned long long)(freeb / 1024));
        return false;
    }

    xSemaphoreTake(g_ctrl_mutex, portMAX_DELAY);
    g_label = label;
    snprintf(g_filename, sizeof(g_filename), "%s/cal_%03u_%s.csv",
             LOGS_DIR, (unsigned)g_next_sample_no, labelName(label));
    g_next_sample_no++;
    g_state = State::START_REQUESTED;
    xSemaphoreGive(g_ctrl_mutex);
    return true;
}

void stopSample() {
    if (g_state == State::RECORDING || g_state == State::START_REQUESTED) {
        g_state = State::STOP_REQUESTED;
    }
}

bool isRecording() {
    return g_state == State::RECORDING || g_state == State::START_REQUESTED;
}

Label currentLabel() { return g_label; }

void appendRow(const Row& r) {
    if (g_state != State::RECORDING || !g_ring) return;

    uint32_t head = g_head;
    // Full means the producer has lapped the consumer by the whole ring.
    if (head - g_tail >= RING_ROWS) {
        g_rows_dropped++;
        return;
    }
    g_ring[head % RING_ROWS] = r;
    g_head = head + 1;
    g_rows_queued++;
}

Stats stats() {
    Stats s;
    s.recording    = isRecording();
    s.label        = g_label;
    s.elapsed_ms   = s.recording ? (millis() - g_start_ms) : 0;
    s.rows_queued  = g_rows_queued;
    s.rows_written = g_rows_written;
    s.rows_dropped = g_rows_dropped;
    s.file_bytes   = g_file_bytes;
    s.free_bytes   = g_free_bytes_cached;
    // Report the bare name, not the full path — it is for a 480px screen.
    const char* slash = strrchr(g_filename, '/');
    snprintf(s.filename, sizeof(s.filename), "%s", slash ? slash + 1 : g_filename);
    return s;
}

} // namespace field_log
