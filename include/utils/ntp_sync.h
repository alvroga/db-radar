#ifndef NTP_SYNC_H
#define NTP_SYNC_H

#include "core/arduino_compat.h"
#include <time.h>
#include "gps_bh880.h"

/**
 * @file ntp_sync.h
 * @brief Hybrid GPS/NTP time synchronization for GPS Radar
 *
 * Features:
 * - GPS-first time sync (most accurate, no WiFi required)
 * - NTP fallback when no GPS signal but WiFi available
 * - Auto-sync when WiFi connects
 * - Update RTC with synced time
 * - Configurable timezone offset
 * - Periodic re-sync to maintain accuracy
 */

namespace ntp_sync {

// =============================================================================
// TIME SOURCE AND STATE TYPES
// =============================================================================

enum class TimeSource {
    RTC_ONLY,   // No sync, using RTC only
    GPS,        // Time synced from GPS (primary)
    NTP         // Time synced from NTP (fallback)
};

enum class TimeSyncState {
    UNSYNCED,           // No valid time yet (boot state)
    SYNCED_QUESTIONABLE, // Using RTC time, but not yet validated by GPS/NTP
    SYNCED_RECENTLY,    // Time synced < 1 hour ago (ignore new syncs)
    STABLE              // Time synced > 1 hour ago (allow resync after 24h)
};

// =============================================================================
// CONFIGURATION
// =============================================================================

constexpr const char* NTP_SERVER_PRIMARY = "pool.ntp.org";
constexpr const char* NTP_SERVER_SECONDARY = "time.nist.gov";
constexpr const char* NTP_SERVER_TERTIARY = "time.google.com";

constexpr uint32_t SYNC_TIMEOUT_MS = 10000;        // 10 second timeout for NTP
constexpr uint32_t GPS_SYNC_TIMEOUT_MS = 120000;   // 2 minute timeout for GPS on boot
constexpr uint32_t QUESTIONABLE_RETRY_MS = 300000; // Retry every 5 minutes when using questionable time
constexpr uint32_t RECENT_SYNC_HOLDOFF_MS = 3600000;    // 1 hour - ignore syncs during this period
constexpr uint32_t STABLE_RESYNC_INTERVAL_MS = 86400000; // 24 hours - resync after stable period

// Timezone configuration (set to your location)
constexpr long GMT_OFFSET_SEC = 0;                  // GMT+0 (UTC)
constexpr int DAYLIGHT_OFFSET_SEC = 0;              // No daylight saving

// =============================================================================
// INITIALIZATION AND CONTROL
// =============================================================================

/**
 * @brief Initialize NTP sync module
 * @param auto_sync_on_wifi Auto-sync when WiFi connects
 */
void init(bool auto_sync_on_wifi = true);

/**
 * @brief Set timezone offset
 * @param gmt_offset_hours GMT offset in hours (e.g., -5 for EST, +1 for CET)
 * @param daylight_offset_hours Daylight saving offset in hours (usually 0 or 1)
 */
void setTimezone(int gmt_offset_hours, int daylight_offset_hours = 0);

// =============================================================================
// TIME SYNCHRONIZATION
// =============================================================================

/**
 * @brief Synchronize time from GPS (primary method)
 * @param gps_data GPS data containing time information
 * @return true if sync successful (requires gps_data.hasTime == true)
 */
bool syncFromGPS(const GPSData& gps_data);

/**
 * @brief Synchronize time with NTP server (fallback method)
 * @param update_rtc If true, update RTC with synced time
 * @return true if sync successful
 */
bool syncTime(bool update_rtc = true);

/**
 * @brief Check if time is synced
 * @return true if system time is valid (synced from NTP)
 */
bool isSynced();

/**
 * @brief Get last successful sync time (millis)
 */
uint32_t getLastSyncMillis();

/**
 * @brief Check if re-sync is needed
 * @return true if it's time to re-sync
 */
bool needsResync();

/**
 * @brief Check if GPS time has been synced (one-shot flag)
 * @return true if GPS time sync has been performed
 */
bool isGPSTimeSynced();

/**
 * @brief Queue GPS time sync via I2C task (safe, non-blocking)
 * @param gps_data GPS data containing time information
 * @return true if sync was queued, false if already synced or invalid data
 */
bool queueGPSTimeSync(const GPSData& gps_data);

/**
 * @brief Update periodic sync (call from main loop or task)
 *
 * Automatically re-syncs if interval expired and WiFi connected.
 */
void update();

// =============================================================================
// TIME ACCESS
// =============================================================================

/**
 * @brief Get current time as struct tm
 * @param tm_struct Output: time structure
 * @return true if time is valid
 */
bool getTime(struct tm& tm_struct);

/**
 * @brief Get current time as formatted string
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @param format Format string (strftime format)
 * @return true if successful
 */
bool getTimeString(char* buffer, size_t buffer_size, const char* format = "%Y-%m-%d %H:%M:%S");

/**
 * @brief Get current Unix timestamp
 * @return Unix timestamp (seconds since 1970-01-01), or 0 if not synced
 */
time_t getUnixTime();

// =============================================================================
// DIAGNOSTICS
// =============================================================================

struct NTPSyncState {
    bool initialized;
    bool auto_sync_enabled;
    bool time_synced;
    TimeSource time_source;               // Current time source (GPS/NTP/RTC_ONLY)
    TimeSyncState sync_state;             // State machine state
    uint32_t boot_time_ms;                // Time when system booted (for timeout)
    uint32_t last_sync_attempt_ms;
    uint32_t last_sync_success_ms;
    uint32_t last_gps_sync_ms;            // Last successful GPS time sync
    uint32_t last_ntp_sync_ms;            // Last successful NTP time sync
    int sync_attempt_count;
    int sync_success_count;
    int sync_failure_count;
    int gps_sync_count;                   // GPS sync success count
    int ntp_sync_count;                   // NTP sync success count
    int gps_rejected_count;               // GPS sync rejected count (validation failed)
    long gmt_offset_sec;
    int daylight_offset_sec;
};

/**
 * @brief Get NTP sync state (for diagnostics)
 */
const NTPSyncState& getState();

/**
 * @brief Print sync status to serial
 */
void printStatus();

} // namespace ntp_sync

#endif // NTP_SYNC_H
