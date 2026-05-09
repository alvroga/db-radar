#include "utils/ntp_sync.h"
#include "utils/system_logger.h"
#include "utils/task_manager.h"
#include "rtc_pcf85063.h"
#include <time.h>
#include <sys/time.h>

// POSIX getLocalTime() replacement for Arduino's getLocalTime()
static bool getLocalTime(struct tm* info) {
    time_t now;
    time(&now);
    if (now < 1577836800L) return false;  // Reject time before 2020-01-01
    localtime_r(&now, info);
    return true;
}

// =============================================================================
// GPS ONE-SHOT TIME SYNC (via I2C queue to avoid bus contention)
// =============================================================================

// One-shot flag to prevent duplicate syncs
static bool g_gps_time_synced = false;

// Buffer for RTC time data (7 bytes: seconds, minutes, hours, days, weekdays, months, years)
static uint8_t g_rtc_time_buffer[7];

// Callback when RTC time set completes
static void onRTCTimeSetComplete(const task_manager::I2CRequest& req) {
    if (req.success) {
        Serial.println("[GPS_TIME] ✓ RTC time synced from GPS (one-shot)");
        system_logger::info("GPS_TIME", "RTC synced from GPS via I2C queue");
        // Notify logger that time has been synced from authoritative source
    } else {
        Serial.println("[GPS_TIME] ✗ RTC time sync failed - will retry on next GPS fix");
        system_logger::error("GPS_TIME", "RTC sync failed via I2C queue");
        // Reset flag to allow retry
        g_gps_time_synced = false;
    }
}

namespace ntp_sync {

// =============================================================================
// INTERNAL STATE
// =============================================================================

static NTPSyncState g_state = {
    .initialized = false,
    .auto_sync_enabled = false,
    .time_synced = false,
    .time_source = TimeSource::RTC_ONLY,
    .sync_state = TimeSyncState::UNSYNCED,
    .boot_time_ms = 0,
    .last_sync_attempt_ms = 0,
    .last_sync_success_ms = 0,
    .last_gps_sync_ms = 0,
    .last_ntp_sync_ms = 0,
    .sync_attempt_count = 0,
    .sync_success_count = 0,
    .sync_failure_count = 0,
    .gps_sync_count = 0,
    .ntp_sync_count = 0,
    .gps_rejected_count = 0,
    .gmt_offset_sec = GMT_OFFSET_SEC,
    .daylight_offset_sec = DAYLIGHT_OFFSET_SEC
};

// =============================================================================
// INTERNAL HELPERS
// =============================================================================

/**
 * @brief Check if we should accept a time sync based on current state
 */
static bool shouldAcceptTimeSync() {
    switch (g_state.sync_state) {
        case TimeSyncState::UNSYNCED:
            // Always accept first sync
            return true;

        case TimeSyncState::SYNCED_QUESTIONABLE:
            // Accept sync attempts (still trying to get good time)
            return true;

        case TimeSyncState::SYNCED_RECENTLY:
            // Reject syncs for 1 hour after successful sync
            {
                uint32_t elapsed = millis() - g_state.last_sync_success_ms;
                return elapsed >= RECENT_SYNC_HOLDOFF_MS;
            }

        case TimeSyncState::STABLE:
            // Only resync after 24 hours
            {
                uint32_t elapsed = millis() - g_state.last_sync_success_ms;
                return elapsed >= STABLE_RESYNC_INTERVAL_MS;
            }

        default:
            return false;
    }
}

/**
 * @brief Update state machine after successful sync
 */
static void updateSyncState(TimeSource source) {
    g_state.time_synced = true;
    g_state.time_source = source;
    g_state.last_sync_success_ms = millis();
    g_state.sync_success_count++;

    // Transition to SYNCED_RECENTLY state
    g_state.sync_state = TimeSyncState::SYNCED_RECENTLY;

    Serial.printf("[TIME] State: SYNCED_RECENTLY (source: %s)\n",
                  source == TimeSource::GPS ? "GPS" : "NTP");
}

static bool updateRTC(const struct tm& time_info, const char* source = "NTP") {
    rtc::Time rtc_time;

    rtc_time.year = time_info.tm_year + 1900;
    rtc_time.month = time_info.tm_mon + 1;
    rtc_time.day = time_info.tm_mday;
    rtc_time.hour = time_info.tm_hour;
    rtc_time.minute = time_info.tm_min;
    rtc_time.second = time_info.tm_sec;
    rtc_time.wday = time_info.tm_wday;
    rtc_time.valid = true;

    bool success = rtc::set(rtc_time);

    if (success) {
        Serial.printf("[TIME] RTC updated from %s: %04d-%02d-%02d %02d:%02d:%02d\n",
                      source,
                      rtc_time.year, rtc_time.month, rtc_time.day,
                      rtc_time.hour, rtc_time.minute, rtc_time.second);
        char log_msg[64];
        snprintf(log_msg, sizeof(log_msg), "RTC updated from %s", source);
        system_logger::info("TIME", log_msg);
    } else {
        Serial.printf("[TIME] Failed to update RTC from %s\n", source);
        system_logger::error("TIME", "Failed to update RTC");
    }

    return success;
}

// =============================================================================
// PUBLIC API - INITIALIZATION
// =============================================================================

void init(bool auto_sync_on_wifi) {
    if (g_state.initialized) {
        return;
    }

    g_state.auto_sync_enabled = auto_sync_on_wifi;
    g_state.initialized = true;
    g_state.boot_time_ms = millis();
    g_state.sync_state = TimeSyncState::UNSYNCED;

    Serial.println("[TIME] State: UNSYNCED - waiting for GPS time sync");
    system_logger::info("TIME", "Time sync module initialized (GPS-only)");
}

void setTimezone(int gmt_offset_hours, int daylight_offset_hours) {
    g_state.gmt_offset_sec = gmt_offset_hours * 3600;
    g_state.daylight_offset_sec = daylight_offset_hours * 3600;
    // NTP removed — timezone stored but not applied (time comes from GPS/RTC)
}

// =============================================================================
// PUBLIC API - TIME SYNCHRONIZATION
// =============================================================================

bool syncFromGPS(const GPSData& gps_data) {
    if (!g_state.initialized) {
        Serial.println("[TIME] GPS sync failed - not initialized");
        return false;
    }

    // Check if GPS has valid time data
    if (!gps_data.hasTime) {
        Serial.println("[TIME] GPS sync failed - no time data");
        return false;
    }

    // Validate GPS time is reasonable (year 2020-2040)
    if (gps_data.year < 2020 || gps_data.year > 2040) {
        // Rate-limit logging to prevent spam (once per 30 seconds max)
        static uint32_t last_reject_log = 0;
        static int reject_count_since_log = 0;
        reject_count_since_log++;

        uint32_t now = millis();
        if (now - last_reject_log >= 30000) {
            last_reject_log = now;
            Serial.printf("[TIME] GPS time invalid: year=%d (rejected %d times)\n",
                         gps_data.year, reject_count_since_log);
            if (gps_data.year == 0) {
                Serial.println("[TIME] Hint: GPS waiting for date from satellites");
            }
            // Only log to file once per 30 seconds to prevent filling log with spam
            char reject_msg[64];
            snprintf(reject_msg, sizeof(reject_msg),
                     "GPS time rejected: year=%d (count=%d)", gps_data.year, reject_count_since_log);
            system_logger::warn("TIME", reject_msg);
            reject_count_since_log = 0;
        }

        g_state.gps_rejected_count++;
        return false;
    }

    // Validate month and day
    if (gps_data.month < 1 || gps_data.month > 12) {
        Serial.printf("[TIME] GPS time invalid: month=%d (must be 1-12)\n", gps_data.month);
        system_logger::error("TIME", "GPS time rejected - invalid month");
        return false;
    }
    if (gps_data.day < 1 || gps_data.day > 31) {
        Serial.printf("[TIME] GPS time invalid: day=%d (must be 1-31)\n", gps_data.day);
        system_logger::error("TIME", "GPS time rejected - invalid day");
        return false;
    }

    // Detect duplicate timestamps (GPS glitch - same time sent repeatedly)
    static uint32_t last_gps_timestamp = 0;
    uint32_t current_timestamp = (gps_data.year * 10000000UL) +
                                  (gps_data.month * 100000UL) +
                                  (gps_data.day * 1000UL) +
                                  (gps_data.hour * 100) +
                                  gps_data.minute;

    if (current_timestamp == last_gps_timestamp && last_gps_timestamp != 0) {
        // Duplicate timestamp - GPS is stuck, ignore
        g_state.gps_rejected_count++;
        return false;
    }
    last_gps_timestamp = current_timestamp;

    // Check state machine - should we accept this sync?
    if (!shouldAcceptTimeSync()) {
        // In holdoff period - ignore sync
        return false;
    }

    Serial.printf("[TIME] GPS time sync: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  gps_data.year, gps_data.month, gps_data.day,
                  gps_data.hour, gps_data.minute, gps_data.second);

    // Convert GPSData to struct tm for RTC update
    struct tm time_info;
    time_info.tm_year = gps_data.year - 1900;  // tm_year is years since 1900
    time_info.tm_mon = gps_data.month - 1;     // tm_mon is 0-11
    time_info.tm_mday = gps_data.day;
    time_info.tm_hour = gps_data.hour;
    time_info.tm_min = gps_data.minute;
    time_info.tm_sec = gps_data.second;
    time_info.tm_wday = 0;  // Will be calculated by mktime
    time_info.tm_isdst = -1; // Unknown DST

    // Update ESP32 system time from GPS (UTC)
    time_t gps_time = mktime(&time_info);
    struct timeval tv = { .tv_sec = gps_time, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    // Update RTC with GPS time
    if (!updateRTC(time_info, "GPS")) {
        Serial.println("[TIME] Warning: RTC update failed, but system time set from GPS");
    }

    // Update state machine
    updateSyncState(TimeSource::GPS);
    g_state.last_gps_sync_ms = millis();
    g_state.gps_sync_count++;

    // Notify logger that time has been synced from authoritative source

    // Check if this is the FIRST GPS sync after boot (time correction event)
    static bool first_gps_sync_logged = false;
    if (!first_gps_sync_logged) {
        first_gps_sync_logged = true;
        system_logger::info("TIME", "========== TIME CORRECTED FROM GPS ==========");
        system_logger::info("TIME", "Log timestamps before this point showed 2000-01-01");

        // Log the correction
        char correction_msg[128];
        snprintf(correction_msg, sizeof(correction_msg),
                 "RTC updated from GPS: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 gps_data.year, gps_data.month, gps_data.day,
                 gps_data.hour, gps_data.minute, gps_data.second);
        system_logger::info("TIME", correction_msg);
    } else {
        // Regular sync message (less verbose)
        char sync_msg[96];
        snprintf(sync_msg, sizeof(sync_msg),
                 "GPS time sync: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 gps_data.year, gps_data.month, gps_data.day,
                 gps_data.hour, gps_data.minute, gps_data.second);
        system_logger::info("TIME", sync_msg);
    }

    Serial.println("[TIME] ✓ GPS time sync successful");
    return true;
}

bool syncTime(bool /*update_rtc*/) {
    // NTP removed — time comes from GPS only
    return false;
}

bool isSynced() {
    return g_state.time_synced;
}

uint32_t getLastSyncMillis() {
    return g_state.last_sync_success_ms;
}

bool needsResync() {
    if (!g_state.time_synced) {
        return true;  // Never synced
    }

    uint32_t elapsed = millis() - g_state.last_sync_success_ms;
    return elapsed >= STABLE_RESYNC_INTERVAL_MS;
}

void update() {
    // NTP removed — no periodic resync needed; GPS time is one-shot at first fix
}

// =============================================================================
// PUBLIC API - TIME ACCESS
// =============================================================================

bool getTime(struct tm& tm_struct) {
    return getLocalTime(&tm_struct);
}

bool getTimeString(char* buffer, size_t buffer_size, const char* format) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        strncpy(buffer, "Time not synced", buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return false;
    }

    strftime(buffer, buffer_size, format, &timeinfo);
    return true;
}

time_t getUnixTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0;
    }

    return mktime(&timeinfo);
}

// =============================================================================
// PUBLIC API - DIAGNOSTICS
// =============================================================================

const NTPSyncState& getState() {
    return g_state;
}

void printStatus() {
    Serial.println("\n=== NTP Sync Status ===");
    Serial.printf("Initialized: %s\n", g_state.initialized ? "yes" : "no");
    Serial.printf("Auto-sync: %s\n", g_state.auto_sync_enabled ? "enabled" : "disabled");
    Serial.printf("Time synced: %s\n", g_state.time_synced ? "yes" : "no");

    // State machine status
    const char* state_str = "UNKNOWN";
    switch (g_state.sync_state) {
        case TimeSyncState::UNSYNCED: state_str = "UNSYNCED"; break;
        case TimeSyncState::SYNCED_QUESTIONABLE: state_str = "SYNCED_QUESTIONABLE"; break;
        case TimeSyncState::SYNCED_RECENTLY: state_str = "SYNCED_RECENTLY"; break;
        case TimeSyncState::STABLE: state_str = "STABLE"; break;
    }
    Serial.printf("Sync state: %s\n", state_str);

    // Time source
    const char* source_str = "UNKNOWN";
    switch (g_state.time_source) {
        case TimeSource::RTC_ONLY: source_str = "RTC_ONLY"; break;
        case TimeSource::GPS: source_str = "GPS"; break;
        case TimeSource::NTP: source_str = "NTP"; break;
    }
    Serial.printf("Time source: %s\n", source_str);

    // Statistics
    Serial.printf("Sync attempts: %d\n", g_state.sync_attempt_count);
    Serial.printf("Sync success: %d (GPS: %d, NTP: %d)\n",
                  g_state.sync_success_count, g_state.gps_sync_count, g_state.ntp_sync_count);
    Serial.printf("Sync failures: %d\n", g_state.sync_failure_count);
    Serial.printf("GPS rejected: %d\n", g_state.gps_rejected_count);

    if (g_state.last_sync_success_ms > 0) {
        uint32_t elapsed_sec = (millis() - g_state.last_sync_success_ms) / 1000;
        Serial.printf("Last sync: %lu seconds ago\n", elapsed_sec);
    } else {
        Serial.println("Last sync: never");
    }

    // Show current time
    char time_str[64];
    if (getTimeString(time_str, sizeof(time_str))) {
        Serial.printf("Current time: %s\n", time_str);
    } else {
        Serial.println("Current time: not available");
    }

    Serial.printf("Timezone: GMT%+ld, DST offset: %dh\n",
                  g_state.gmt_offset_sec / 3600,
                  g_state.daylight_offset_sec / 3600);

    Serial.println("=====================\n");
}

// =============================================================================
// GPS ONE-SHOT TIME SYNC API
// =============================================================================

bool isGPSTimeSynced() {
    return g_gps_time_synced;
}

bool queueGPSTimeSync(const GPSData& gps_data) {
    // Check if already synced (one-shot)
    if (g_gps_time_synced) {
        return false;
    }

    // Validate GPS time data
    if (!gps_data.hasTime) {
        return false;
    }

    // Validate year range (2020-2040)
    if (gps_data.year < 2020 || gps_data.year > 2040) {
        return false;
    }

    // Validate month and day
    if (gps_data.month < 1 || gps_data.month > 12) {
        return false;
    }
    if (gps_data.day < 1 || gps_data.day > 31) {
        return false;
    }

    // Set flag immediately to prevent duplicate queue attempts
    g_gps_time_synced = true;

    Serial.printf("[GPS_TIME] Queued RTC sync: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  gps_data.year, gps_data.month, gps_data.day,
                  gps_data.hour, gps_data.minute, gps_data.second);

    // Build RTC time data in BCD format for PCF85063
    // Register order: 0x04=seconds, 0x05=minutes, 0x06=hours, 0x07=days, 0x08=weekdays, 0x09=months, 0x0A=years
    auto toBCD = [](uint8_t val) -> uint8_t {
        return ((val / 10) << 4) | (val % 10);
    };

    g_rtc_time_buffer[0] = toBCD(gps_data.second);       // Seconds (0x04)
    g_rtc_time_buffer[1] = toBCD(gps_data.minute);       // Minutes (0x05)
    g_rtc_time_buffer[2] = toBCD(gps_data.hour);         // Hours (0x06)
    g_rtc_time_buffer[3] = toBCD(gps_data.day);          // Days (0x07)
    g_rtc_time_buffer[4] = 0;                            // Weekdays (0x08) - don't care
    g_rtc_time_buffer[5] = toBCD(gps_data.month);        // Months (0x09)
    g_rtc_time_buffer[6] = toBCD(gps_data.year - 2000);  // Years (0x0A) - offset from 2000

    // Queue I2C request to write RTC time
    task_manager::I2CRequest rtc_request;
    rtc_request.device = task_manager::I2CDeviceType::RTC;
    rtc_request.operation = task_manager::I2COperation::RTC_TIME_SET;
    rtc_request.device_addr = 0x51;     // PCF85063 address
    rtc_request.reg_addr = 0x04;        // Time register start
    rtc_request.data = g_rtc_time_buffer;
    rtc_request.data_len = 7;
    rtc_request.success = false;
    rtc_request.timestamp = millis();
    rtc_request.requester_task = nullptr;
    rtc_request.callback = onRTCTimeSetComplete;

    if (!task_manager::queueI2CRequest(rtc_request)) {
        Serial.println("[GPS_TIME] Failed to queue RTC sync - queue full");
        g_gps_time_synced = false;  // Reset to allow retry
        return false;
    }

    // Also update ESP32 system time immediately (doesn't need I2C)
    struct tm time_info;
    time_info.tm_year = gps_data.year - 1900;
    time_info.tm_mon = gps_data.month - 1;
    time_info.tm_mday = gps_data.day;
    time_info.tm_hour = gps_data.hour;
    time_info.tm_min = gps_data.minute;
    time_info.tm_sec = gps_data.second;
    time_info.tm_wday = 0;
    time_info.tm_isdst = -1;

    time_t gps_time = mktime(&time_info);
    struct timeval tv = { .tv_sec = gps_time, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    // Update internal state
    g_state.time_synced = true;
    g_state.time_source = TimeSource::GPS;
    g_state.sync_state = TimeSyncState::SYNCED_RECENTLY;
    g_state.last_sync_success_ms = millis();
    g_state.last_gps_sync_ms = millis();
    g_state.gps_sync_count++;
    g_state.sync_success_count++;

    system_logger::info("GPS_TIME", "System time set, RTC sync queued");

    return true;
}

} // namespace ntp_sync
