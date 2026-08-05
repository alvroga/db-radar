#include "utils/system_logger.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include "esp_system.h"

namespace system_logger {

// =============================================================================
// STATE
// =============================================================================

static bool g_initialized = false;
static bool g_enabled = true;
static char* g_buffer = nullptr;
static size_t g_buffer_pos = 0;
static uint32_t g_last_flush_ms = 0;
static size_t g_file_size = 0;
static SemaphoreHandle_t g_mutex = nullptr;

// =============================================================================
// HELPERS
// =============================================================================

static const char* levelToString(Level level) {
    switch (level) {
        case Level::DEBUG: return "D";
        case Level::INFO:  return "I";
        case Level::WARN:  return "W";
        case Level::ERROR: return "E";
        default:           return "?";
    }
}

// Uptime (HH:MM:SS.mmm) until the system clock is actually valid, then real UTC
// date/time. ntp_sync.cpp calls settimeofday() once from a GPS fix (no signal back
// to this module needed) — 1577836800 = 2020-01-01 00:00:00 UTC is the same
// not-yet-synced sanity gate ntp_sync.cpp's own getLocalTime() already uses, so a
// log captured before the first fix and one captured after are self-consistent
// with every other UTC print in this codebase (GPS_TIME's own boot line, WMM, etc).
// Mid-file format changes at the fix are intentional, not a bug: it's the marker
// for "GPS fix acquired" in the log itself.
static void getTimestamp(char* buf, size_t size) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec >= 1577836800L) {
        struct tm tm_info;
        gmtime_r(&tv.tv_sec, &tm_info);
        snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ldZ",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                 (long)(tv.tv_usec / 1000));
        return;
    }

    uint32_t ms = millis();
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    uint32_t hr = min / 60;
    snprintf(buf, size, "%02lu:%02lu:%02lu.%03lu",
             hr % 24, min % 60, sec % 60, ms % 1000);
}

// Shifts system.log -> system_1.log -> system_2.log -> ... -> system_N.log, dropping
// whatever was in the oldest slot. Called once at boot, before the fresh file for this
// session is opened, so every boot gets its own file rather than appending forever into
// one that can be truncated by MAX_LOG_SIZE during a later, unrelated session.
static void rotateLogs() {
    char oldest[64];
    snprintf(oldest, sizeof(oldest), "/sdcard/logs/system_%d.log", MAX_ROTATED_LOGS);
    remove(oldest);  // no-op if it doesn't exist

    for (int i = MAX_ROTATED_LOGS - 1; i >= 1; i--) {
        char from[64], to[64];
        snprintf(from, sizeof(from), "/sdcard/logs/system_%d.log", i);
        snprintf(to, sizeof(to), "/sdcard/logs/system_%d.log", i + 1);
        rename(from, to);  // no-op (fails silently) if `from` doesn't exist
    }

    rename(LOG_FILE, "/sdcard/logs/system_1.log");
}

// Distinguishes "the firmware reset" from "the UI hung with no reset" — the two look
// identical from the outside (screen stops updating) but point at completely different
// bug classes. See docs/i2c_bus_freeze_investigation.md.
static const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// =============================================================================
// PUBLIC API
// =============================================================================

bool init() {
    if (g_initialized) {
        return true;
    }

    // Heap-allocate the buffer — only consumes SRAM when logging is actually active
    g_buffer = (char*)malloc(BUFFER_SIZE);
    if (!g_buffer) {
        Serial.println("[LOG] Failed to allocate log buffer");
        return false;
    }

    // Create mutex for thread safety
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        Serial.println("[LOG] Failed to create mutex");
        free(g_buffer);
        g_buffer = nullptr;
        return false;
    }

    // Ensure logs directory exists
    struct stat st;
    if (stat("/sdcard/logs", &st) != 0) {
        if (mkdir("/sdcard/logs", 0777) != 0) {
            Serial.println("[LOG] Failed to create /sdcard/logs directory");
            return false;
        }
    }

    // Rotate: preserve whatever the previous boot wrote (which may be the last
    // thing logged before a crash) into system_1.log, untouched by this session.
    rotateLogs();

    // Check existing file size (should be 0/absent right after rotation — this stays
    // as a safety net in case rotation itself failed, e.g. a full SD card)
    if (stat(LOG_FILE, &st) == 0) {
        g_file_size = (size_t)st.st_size;
        // If file too large, truncate it
        if (g_file_size >= MAX_LOG_SIZE) {
            remove(LOG_FILE);
            g_file_size = 0;
            Serial.println("[LOG] Truncated oversized log file");
        }
    }

    g_buffer_pos = 0;
    g_last_flush_ms = millis();
    g_initialized = true;

    Serial.printf("[LOG] Initialized (file: %d bytes, buffer: %d bytes)\n",
                  g_file_size, BUFFER_SIZE);

    // Log boot, and WHY — a task/interrupt watchdog reset and a true UI hang look
    // identical from outside (screen stops updating) but implicate different code.
    // A hang with reset_reason still POWERON across boots means nothing reset it —
    // the user power-cycled it, which is a different fact than the firmware crashing.
    log(Level::INFO, "SYS", "===== BOOT =====");
    logf(Level::INFO, "SYS", "Reset reason: %s", resetReasonToString(esp_reset_reason()));

    return true;
}

void log(Level level, const char* tag, const char* message) {
    if (!g_initialized || !g_enabled) {
        return;
    }

    // Format: "HH:MM:SS.mmm [L] TAG: message\n" (pre-fix) or
    // "YYYY-MM-DD HH:MM:SS.mmmZ [L] TAG: message\n" (post-GPS-fix) — see getTimestamp().
    char timestamp[32];
    getTimestamp(timestamp, sizeof(timestamp));

    char line[256];
    int len = snprintf(line, sizeof(line), "%s [%s] %s: %s\n",
                       timestamp, levelToString(level), tag, message);

    if (len <= 0 || len >= (int)sizeof(line)) {
        return;  // Format error or truncation
    }

    // Thread-safe buffer append
    if (g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Check if fits in buffer
        if (g_buffer_pos + len < BUFFER_SIZE) {
            memcpy(g_buffer + g_buffer_pos, line, len);
            g_buffer_pos += len;
        }
        // If buffer full, drop the message (non-blocking design)
        // The periodic flush will make room

        xSemaphoreGive(g_mutex);
    }

    // Also print errors to serial
    if (level >= Level::ERROR) {
        Serial.print(line);
    }
}

void logf(Level level, const char* tag, const char* fmt, ...) {
    char message[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    log(level, tag, message);
}

bool flush() {
    if (!g_initialized || g_buffer_pos == 0) {
        g_last_flush_ms = millis();
        return true;
    }

    // Take mutex with longer timeout for flush
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("[LOG] Flush mutex timeout");
        return false;
    }

    bool success = false;
    size_t to_write = g_buffer_pos;

    // Check if we need to truncate file first
    if (g_file_size + to_write > MAX_LOG_SIZE) {
        remove(LOG_FILE);
        g_file_size = 0;
        Serial.println("[LOG] Log file rotated (size limit)");
    }

    // Open file in append mode
    FILE* f = fopen(LOG_FILE, "ab");
    if (f) {
        size_t written = fwrite(g_buffer, 1, to_write, f);
        fclose(f);

        if (written == to_write) {
            g_file_size += written;
            g_buffer_pos = 0;
            success = true;
        } else {
            Serial.printf("[LOG] Partial write: %d/%d\n", (int)written, (int)to_write);
        }
    } else {
        Serial.println("[LOG] Failed to open log file");
    }

    g_last_flush_ms = millis();
    xSemaphoreGive(g_mutex);

    return success;
}

bool needsFlush() {
    return g_initialized &&
           (millis() - g_last_flush_ms >= FLUSH_INTERVAL_MS) &&
           (g_buffer_pos > 0);
}

void setEnabled(bool enabled) {
    g_enabled = enabled;
}

bool isEnabled() {
    return g_enabled;
}

size_t getBufferUsage() {
    return g_buffer_pos;
}

size_t getFileSize() {
    return g_file_size;
}

void close() {
    if (!g_initialized) {
        return;
    }

    flush();
    g_initialized = false;

    if (g_mutex) {
        vSemaphoreDelete(g_mutex);
        g_mutex = nullptr;
    }

    if (g_buffer) {
        free(g_buffer);
        g_buffer = nullptr;
    }

    Serial.println("[LOG] Closed");
}

} // namespace system_logger
