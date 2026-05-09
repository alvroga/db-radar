#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include "core/arduino_compat.h"

namespace settings_manager {

static constexpr int BEACON_MAX = 7;

// Radar settings structure
struct RadarSettings {
    // Radar display settings
    uint8_t default_zoom_level = 3;     // 0-4: 10km, 1km, 500m, 100m (default), 10m
    bool grid_visible = true;
    uint8_t grid_color = 0;             // 0=black, 1=white, 2=gray

    // GPS settings
    uint16_t gps_update_interval_ms = 200;    // 5Hz default (walking/hiking). 10Hz=100, 2Hz=500, 1Hz=1000
    uint16_t gps_log_interval_sec = 10;       // Log every 10 seconds
    bool gps_logging_enabled = true;

    // GPS state for fast fix (hot/warm start)
    double gps_last_lat = 0.0;                // Last known latitude
    double gps_last_lon = 0.0;                // Last known longitude
    uint32_t gps_last_fix_time = 0;           // Unix timestamp of last valid fix (seconds since epoch)
    bool gps_has_saved_position = false;      // True if we have a saved position

    // Waypoint settings
    bool waypoints_persistent = true;    // Save waypoints to NVS
    uint8_t max_visible_waypoints = 50;

    // Display settings
    uint8_t brightness = 128;            // 0-255
    bool auto_brightness = false;
    uint8_t theme = 0;                   // 0=green (default), 1=blue, 2=night
    bool heading_up_mode = true;         // Navigation mode: true=heading-up, false=north-up
    bool hud_auto_hide = true;           // HUD auto-hide enabled (default: ON)
    uint16_t hud_auto_hide_seconds = 10; // Auto-hide delay in seconds (5, 10, 20, 30, 60, 0=never)
    bool daylight_mode = false;          // Daylight mode: high contrast colors for outdoor use
    bool north_indicator_enabled = true; // Show N indicator in heading-up mode
    uint8_t auto_sleep_timeout_minutes = 0;  // Auto sleep after inactivity: 0=off, 5, 10, 15, 30

    // Connectivity settings
    bool wifi_enabled = false;           // WiFi STA session flag (never persisted, always boots false)
    bool wifi_ap_enabled = false;        // AP upload mode boot flag (NVS-persisted via wifi_ap_en)
    bool wifi_sta_boot = false;          // WiFi STA mode boot flag (NVS-persisted via wifi_sta_en)
    char ap_ssid[33]     = "Radar-GPX"; // AP mode SSID (user-configurable, NVS-persisted)
    char ap_password[64] = "radar123";  // AP mode password (user-configurable, NVS-persisted)

    // GPX management
    char active_gpx_file[64] = "";       // Currently loaded GPX file path
    bool gpx_auto_load = false;          // Auto-load last GPX on boot

    // Sound settings
    bool button_sound_enabled = false;   // Button press sound enabled (default: OFF, for diagnostics)

    // Developer settings
    bool dev_mode = false;               // Master dev mode toggle (default: OFF for release)
    bool dev_tab_visible = false;        // DEV tab visible in settings (default: OFF for release)
    bool logging_enabled = false;        // System event logging enabled (default: OFF for release)

    // Compass calibration (hard iron offsets)
    int16_t compass_cal_x = 0;
    int16_t compass_cal_y = 0;
    int16_t compass_cal_z = 0;
    bool compass_calibrated = false;

    // Magnetic declination (auto-computed from WMM at GPS fix, cached in NVS)
    // Apply: true_heading = magnetic_heading - compass_declination_deg
    float compass_declination_deg = 0.0f;   // Degrees East (positive = East)
    bool  compass_declination_valid = false; // True once computed from a GPS fix

    // Beacon proximity settings
    bool beacon_sound_enabled = true;                         // Beacon proximity sound enabled (default: ON)
    int beacon_count = 0;                                     // Number of configured beacons (0 = none)
    char beacon_macs[BEACON_MAX][18];                         // MAC addresses (XX:XX:XX:XX:XX:XX)
    char beacon_names[BEACON_MAX][16];                        // Display names (optional, may be empty)
    int8_t beacon_measured_power = -59;                       // Calibrated RSSI at 1m (global, all beacons)
    float beacon_path_loss_n = 2.5f;                          // Path loss exponent (global)

    // Keep legacy field for migration (loaded from NVS only, not used at runtime)
    bool beacon_proximity_enabled = true;

    // Beacon found/missing state (persisted so it survives power cycle)
    bool beacon_found = false;
};

/**
 * @brief Initialize settings manager and NVS storage
 * Loads settings from NVS into RAM cache for fast access.
 * @return true on success, false on failure
 */
bool init();

/**
 * @brief Get cached settings (fast, no I/O)
 * Returns reference to RAM-cached settings. Use this for all reads.
 * Settings are loaded once at boot and updated by saveSettings().
 * @return const reference to cached settings
 */
const RadarSettings& getSettings();

/**
 * @brief Load all settings from NVS (internal use only)
 * Called once at boot by init(). Use getSettings() for normal access.
 * @param settings Output parameter for loaded settings
 * @return true on success, false on failure
 */
bool loadSettings(RadarSettings& settings);

/**
 * @brief Save all settings to NVS
 * @param settings Settings to save
 * @return true on success, false on failure
 */
bool saveSettings(const RadarSettings& settings);

/**
 * @brief Get default settings (factory reset)
 * @param settings Output parameter for default settings
 */
void getDefaultSettings(RadarSettings& settings);

/**
 * @brief Reset all settings to factory defaults
 * @return true on success, false on failure
 */
bool resetToDefaults();

/**
 * @brief Print current settings to serial (for debugging)
 */
void printSettings();

/**
 * @brief Save individual radar setting
 */
bool saveZoomLevel(uint8_t zoom_level);
bool saveGridVisibility(bool visible);
bool saveGridColor(uint8_t color);

/**
 * @brief Save individual GPS setting
 */
bool saveGPSUpdateInterval(uint16_t interval_ms);
bool saveGPSLogInterval(uint16_t interval_sec);
bool saveGPSLoggingEnabled(bool enabled);
/**
 * @brief Save GPS state for fast fix on next boot
 * @param lat Last known latitude
 * @param lon Last known longitude
 * @param fix_time Unix timestamp of the fix
 * @return true on success
 */
bool saveGPSState(double lat, double lon, uint32_t fix_time);

/**
 * @brief Load GPS state for determining hot/warm start
 * @param lat Output: Last known latitude
 * @param lon Output: Last known longitude
 * @param fix_time Output: Unix timestamp of last fix
 * @return true if valid saved state exists
 */
bool loadGPSState(double& lat, double& lon, uint32_t& fix_time);

/**
 * @brief Save individual waypoint setting
 */
bool saveWaypointsPersistent(bool persistent);
bool saveMaxVisibleWaypoints(uint8_t max_waypoints);

/**
 * @brief Save individual display setting
 */
bool saveBrightness(uint8_t brightness);
bool saveAutoBrightness(bool enabled);
bool saveTheme(uint8_t theme);
bool saveHeadingUpMode(bool heading_up);
bool saveHUDAutoHide(bool enabled);
bool saveHUDAutoHideSeconds(uint16_t seconds);
bool saveDaylightMode(bool enabled);
bool saveNorthIndicatorEnabled(bool enabled);
bool saveAutoSleepTimeout(uint8_t minutes);

/**
 * @brief Save individual connectivity setting
 */
bool saveWiFiEnabled(bool enabled);
bool saveWiFiAPEnabled(bool enabled);
bool saveWiFiSTABoot(bool enabled);     // Persists wifi_sta_boot to NVS (wifi_sta_en key)
bool saveAPSSID(const char* ssid);      // Persists AP SSID to NVS
bool saveAPPassword(const char* pass);  // Persists AP password to NVS
bool prepareForOTAReboot();             // Invalidates fw_stamp + clears WiFi boot flags atomically

/**
 * @brief Save GPX management setting
 */
bool saveActiveGPXFile(const char* filepath);
bool saveGPXAutoLoad(bool auto_load);

/**
 * @brief Save sound settings
 */
bool saveButtonSoundEnabled(bool enabled);

/**
 * @brief Save developer settings
 */
bool saveDevMode(bool enabled);
bool saveDevTabVisible(bool visible);
bool saveLoggingEnabled(bool enabled);

/**
 * @brief Save magnetic declination computed from WMM
 * @param declination_deg Degrees East (positive = East)
 */
bool saveDeclination(float declination_deg);

/**
 * @brief Beacon proximity settings
 */
bool saveCompassCalibration(int16_t x, int16_t y, int16_t z);

bool saveBeaconProximityEnabled(bool enabled);
bool saveBeaconSoundEnabled(bool enabled);
bool saveBeaconMAC(const char* mac);
bool saveBeaconMeasuredPower(int8_t power);
bool saveBeaconPathLoss(float n);
// NOTE: saveBeaconEntry/deleteBeaconEntry/saveBeaconCount are reserved for
// multi-beacon management UI (tickets/plan_beacon_management.md) — not yet implemented.
bool saveBeaconFound(bool found);

/**
 * @brief WiFi credential management
 * WiFi credentials are stored in a separate NVS namespace "wifi"
 */
bool saveWiFi(const String& ssid, const String& password);
bool getWiFi(String& ssid, String& password);
bool hasWiFi();
bool clearWiFi();

} // namespace settings_manager

#endif // SETTINGS_MANAGER_H
