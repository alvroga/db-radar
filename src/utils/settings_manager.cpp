#include "settings_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "core/fw_version_gen.h"

namespace settings_manager {

// NVS namespace names
static const char* NAMESPACE_SETTINGS = "radar";
static const char* NAMESPACE_WIFI     = "wifi";

// NVS keys (radar namespace)
static const char* KEY_ZOOM_LEVEL = "zoom_level";
static const char* KEY_GRID_VISIBLE = "grid_vis";
static const char* KEY_GRID_COLOR = "grid_color";
static const char* KEY_GPS_UPDATE = "gps_update";
static const char* KEY_GPS_LOG = "gps_log";
static const char* KEY_GPS_LOG_EN = "gps_log_en";
static const char* KEY_GPS_LAST_LAT = "gps_lat";
static const char* KEY_GPS_LAST_LON = "gps_lon";
static const char* KEY_GPS_LAST_FIX = "gps_fix";
static const char* KEY_GPS_HAS_POS = "gps_haspos";
static const char* KEY_WP_PERSIST = "wp_persist";
static const char* KEY_WP_MAX = "wp_max";
static const char* KEY_BRIGHTNESS = "brightness";
static const char* KEY_AUTO_BRIGHT = "auto_bright";
static const char* KEY_THEME = "theme";
static const char* KEY_HUD_AUTO = "hud_auto";
static const char* KEY_HUD_DELAY = "hud_delay";
static const char* KEY_GPX_FILE = "gpx_file";
static const char* KEY_GPX_AUTO = "gpx_auto";
static const char* KEY_DEV_MODE = "dev_mode";
static const char* KEY_DEV_TAB = "dev_tab";
static const char* KEY_LOGGING_EN = "logging_en";
static const char* KEY_DAYLIGHT = "daylight";
static const char* KEY_NORTH_IND = "north_ind";
static const char* KEY_BTN_SOUND = "btn_sound";
static const char* KEY_AUTO_SLEEP = "auto_sleep";
static const char* KEY_BCN_SND   = "bcn_snd";
static const char* KEY_BCN_MAC   = "bcn_mac";
static const char* KEY_BCN_PWR   = "bcn_pwr";
static const char* KEY_BCN_N     = "bcn_n";
static const char* KEY_BCN_FOUND = "bcn_found";
static const char* KEY_HEADING_UP = "heading_up";
static const char* KEY_COMPASS_CAL_X  = "cal_cx";
static const char* KEY_COMPASS_CAL_Y  = "cal_cy";
static const char* KEY_COMPASS_CAL_Z  = "cal_cz";
static const char* KEY_COMPASS_CALED  = "cal_done";
static const char* KEY_DECL_DEG   = "decl_deg";
static const char* KEY_DECL_VALID = "decl_valid";
static const char* KEY_WIFI_AP_EN  = "wifi_ap_en";
static const char* KEY_WIFI_STA_EN = "wifi_sta_en";
static const char* KEY_AP_SSID     = "ap_ssid";
static const char* KEY_AP_PASS     = "ap_pass";
// Firmware stamp — if missing or mismatched, boot-mode flags are cleared (safe after reflash).
// FW_BUILD_TS is a Unix timestamp written by scripts/gen_version.py on every build.
// Because it changes on every build, settings_manager.cpp always recompiles, and any
// USB or OTA flash produces a stamp mismatch on first boot → radar mode guaranteed.
static const char* KEY_FW_STAMP = "fw_stamp";
static const uint32_t FW_STAMP_VAL = FW_BUILD_TS;

// NVS keys (wifi namespace)
static const char* KEY_WIFI_SSID = "ssid";
static const char* KEY_WIFI_PASSWORD = "password";

static bool is_initialized = false;
static RadarSettings g_cached_settings;

// ============================================================================
// NVS helper functions (mirror Preferences API)
// ============================================================================

static nvs_handle_t nvs_open_ns(const char* ns, bool readonly) {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(ns, readonly ? NVS_READONLY : NVS_READWRITE, &h);
    if (err != ESP_OK) return 0;
    return h;
}
static void nvs_close_ns(nvs_handle_t h, bool commit) {
    if (h == 0) return;
    if (commit) nvs_commit(h);
    nvs_close(h);
}

static uint8_t  nvs_getu8(nvs_handle_t h, const char* k, uint8_t def)   { if(!h) return def; uint8_t v=def; nvs_get_u8(h,k,&v); return v; }
static uint16_t nvs_getu16(nvs_handle_t h, const char* k, uint16_t def) { if(!h) return def; uint16_t v=def; nvs_get_u16(h,k,&v); return v; }
static uint32_t nvs_getu32(nvs_handle_t h, const char* k, uint32_t def) { if(!h) return def; uint32_t v=def; nvs_get_u32(h,k,&v); return v; }
static int8_t   nvs_geti8(nvs_handle_t h, const char* k, int8_t def)    { if(!h) return def; int8_t v=def; nvs_get_i8(h,k,&v); return v; }
static int16_t  nvs_geti16(nvs_handle_t h, const char* k, int16_t def)  { if(!h) return def; int16_t v=def; nvs_get_i16(h,k,&v); return v; }
static bool     nvs_getbool(nvs_handle_t h, const char* k, bool def)     { if(!h) return def; uint8_t v=def?1:0; nvs_get_u8(h,k,&v); return v!=0; }

static float nvs_getfloat(nvs_handle_t h, const char* k, float def) {
    if (!h) return def;
    uint32_t bits; memcpy(&bits, &def, 4);
    nvs_get_u32(h, k, &bits);
    float v; memcpy(&v, &bits, 4);
    return v;
}
static double nvs_getdouble(nvs_handle_t h, const char* k, double def) {
    if (!h) return def;
    uint64_t bits; memcpy(&bits, &def, 8);
    nvs_get_u64(h, k, &bits);
    double v; memcpy(&v, &bits, 8);
    return v;
}
static void nvs_getstr(nvs_handle_t h, const char* k, char* buf, size_t n) {
    if (!h || !buf) return;
    // Do NOT clear buf upfront — preserve the struct default if key not in NVS
    size_t len = n;
    nvs_get_str(h, k, buf, &len);
}

static void nvs_setbool(nvs_handle_t h, const char* k, bool v)    { if(h) nvs_set_u8(h, k, v?1:0); }
static void nvs_setfloat(nvs_handle_t h, const char* k, float v)  { if(h) { uint32_t b; memcpy(&b,&v,4); nvs_set_u32(h,k,b); } }
static void nvs_setdouble(nvs_handle_t h, const char* k, double v){ if(h) { uint64_t b; memcpy(&b,&v,8); nvs_set_u64(h,k,b); } }

// ============================================================================
// Init
// ============================================================================

bool init() {
    if (is_initialized) {
        Serial.println("[SETTINGS] Already initialized");
        return true;
    }

    Serial.println("[SETTINGS] Initializing settings manager...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[SETTINGS] NVS partition needs formatting, erasing...");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            Serial.printf("[SETTINGS] ERROR: Failed to erase NVS: %s\n", esp_err_to_name(err));
            return false;
        }
        err = nvs_flash_init();
        if (err != ESP_OK) {
            Serial.printf("[SETTINGS] ERROR: Failed to re-initialize NVS: %s\n", esp_err_to_name(err));
            return false;
        }
        Serial.println("[SETTINGS] NVS partition formatted successfully");
    } else if (err != ESP_OK) {
        Serial.printf("[SETTINGS] ERROR: NVS init failed: %s\n", esp_err_to_name(err));
        return false;
    } else {
        Serial.println("[SETTINGS] NVS already initialized");
    }

    // Verify namespace is accessible
    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false);
    if (!h) {
        Serial.println("[SETTINGS] ERROR: Failed to open NVS namespace");
        return false;
    }
    nvs_close_ns(h, true);
    Serial.println("[SETTINGS] NVS namespace opened/created successfully");

    is_initialized = true;

    if (!loadSettings(g_cached_settings)) {
        Serial.println("[SETTINGS] WARNING: Could not load settings, using defaults");
        getDefaultSettings(g_cached_settings);
    }

    Serial.println("[SETTINGS] Initialization complete (settings cached in RAM)");
    printSettings();
    return true;
}

const RadarSettings& getSettings() { return g_cached_settings; }

void getDefaultSettings(RadarSettings& settings) {
    settings = RadarSettings();
    strncpy(settings.beacon_macs[0], "DD:34:02:0A:26:7C", sizeof(settings.beacon_macs[0]) - 1);
    settings.beacon_count = 1;
}

bool loadSettings(RadarSettings& settings) {
    if (!is_initialized) {
        Serial.println("[SETTINGS] ERROR: Not initialized, call init() first");
        return false;
    }

    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, true);
    if (!h) {
        Serial.println("[SETTINGS] ERROR: Failed to open NVS for reading");
        return false;
    }

    settings.default_zoom_level           = nvs_getu8(h, KEY_ZOOM_LEVEL, 3);
    settings.grid_visible                 = nvs_getbool(h, KEY_GRID_VISIBLE, true);
    settings.grid_color                   = nvs_getu8(h, KEY_GRID_COLOR, 0);
    settings.gps_update_interval_ms       = nvs_getu16(h, KEY_GPS_UPDATE, 200);
    settings.gps_log_interval_sec         = nvs_getu16(h, KEY_GPS_LOG, 10);
    settings.gps_logging_enabled          = nvs_getbool(h, KEY_GPS_LOG_EN, true);
    settings.gps_has_saved_position       = nvs_getbool(h, KEY_GPS_HAS_POS, false);
    settings.gps_last_lat                 = nvs_getdouble(h, KEY_GPS_LAST_LAT, 0.0);
    settings.gps_last_lon                 = nvs_getdouble(h, KEY_GPS_LAST_LON, 0.0);
    settings.gps_last_fix_time            = nvs_getu32(h, KEY_GPS_LAST_FIX, 0);
    settings.waypoints_persistent         = nvs_getbool(h, KEY_WP_PERSIST, true);
    settings.max_visible_waypoints        = nvs_getu8(h, KEY_WP_MAX, 50);
    settings.brightness                   = nvs_getu8(h, KEY_BRIGHTNESS, 128);
    settings.auto_brightness              = nvs_getbool(h, KEY_AUTO_BRIGHT, false);
    settings.theme                        = nvs_getu8(h, KEY_THEME, 0);
    settings.hud_auto_hide                = nvs_getbool(h, KEY_HUD_AUTO, true);
    settings.hud_auto_hide_seconds        = nvs_getu16(h, KEY_HUD_DELAY, 10);
    settings.daylight_mode                = nvs_getbool(h, KEY_DAYLIGHT, false);
    settings.north_indicator_enabled      = nvs_getbool(h, KEY_NORTH_IND, true);
    settings.auto_sleep_timeout_minutes   = nvs_getu8(h, KEY_AUTO_SLEEP, 0);
    // wifi_enabled is session-only — never loaded from NVS, always boots as false
    // settings.wifi_enabled stays at the struct default (false)
    // wifi_ap_enabled: only trust NVS value if firmware stamp matches (guards against
    // stale NVS after reflash — fresh flash always boots to radar mode).
    {
        uint32_t stamp = nvs_getu32(h, KEY_FW_STAMP, 0);
        if (stamp == FW_STAMP_VAL) {
            settings.wifi_ap_enabled = nvs_getbool(h, KEY_WIFI_AP_EN, false);
            settings.wifi_sta_boot   = nvs_getbool(h, KEY_WIFI_STA_EN, false);
        } else {
            Serial.println("[SETTINGS] Fresh firmware detected — resetting boot-mode flags to off");
            settings.wifi_ap_enabled = false;
            settings.wifi_sta_boot   = false;
            // Write the stamp (requires read-write handle — close and reopen)
            nvs_close_ns(h, false);
            nvs_handle_t hw = nvs_open_ns(NAMESPACE_SETTINGS, false);
            if (hw) {
                nvs_set_u32(hw, KEY_FW_STAMP, FW_STAMP_VAL);
                nvs_setbool(hw, KEY_WIFI_AP_EN, false);
                nvs_setbool(hw, KEY_WIFI_STA_EN, false);
                nvs_close_ns(hw, true);
            }
            h = nvs_open_ns(NAMESPACE_SETTINGS, true); // re-open for remaining reads
            if (!h) {
                Serial.println("[SETTINGS] WARN: Could not re-open NVS after stamp write");
                return true; // continue with what we have
            }
        }
    }
    // AP credentials — always load regardless of stamp (user config, not a boot-mode flag)
    nvs_getstr(h, KEY_AP_SSID, settings.ap_ssid, sizeof(settings.ap_ssid));
    if (settings.ap_ssid[0] == '\0') strncpy(settings.ap_ssid, "Radar-GPX", sizeof(settings.ap_ssid) - 1);
    nvs_getstr(h, KEY_AP_PASS, settings.ap_password, sizeof(settings.ap_password));
    if (settings.ap_password[0] == '\0') strncpy(settings.ap_password, "radar123", sizeof(settings.ap_password) - 1);
    nvs_getstr(h, KEY_GPX_FILE, settings.active_gpx_file, sizeof(settings.active_gpx_file));
    settings.gpx_auto_load                = nvs_getbool(h, KEY_GPX_AUTO, false);
    settings.button_sound_enabled         = nvs_getbool(h, KEY_BTN_SOUND, false);
    settings.dev_mode                     = nvs_getbool(h, KEY_DEV_MODE, true);
    settings.dev_tab_visible              = nvs_getbool(h, KEY_DEV_TAB, true);
    settings.logging_enabled              = nvs_getbool(h, KEY_LOGGING_EN, true);
    settings.heading_up_mode              = nvs_getbool(h, KEY_HEADING_UP, true);
    // beacon_proximity_enabled: always on (MAC is hardcoded, feature is always active)
    // Don't load from NVS — was previously saved as false, would block the feature
    settings.beacon_sound_enabled         = nvs_getbool(h, KEY_BCN_SND, true);
    settings.beacon_measured_power        = nvs_geti8(h, KEY_BCN_PWR, -59);
    settings.beacon_path_loss_n           = nvs_getfloat(h, KEY_BCN_N, 2.5f);
    // Load beacon list from NVS (migrate from legacy single-MAC key if needed)
    nvs_getstr(h, KEY_BCN_MAC, settings.beacon_macs[0], sizeof(settings.beacon_macs[0]));
    if (strlen(settings.beacon_macs[0]) > 0) {
        settings.beacon_count = 1;
    } else {
        // First boot: seed hardcoded default MAC
        strncpy(settings.beacon_macs[0], "DD:34:02:0A:26:7C", sizeof(settings.beacon_macs[0]) - 1);
        settings.beacon_count = 1;
    }
    settings.beacon_found = nvs_getbool(h, KEY_BCN_FOUND, false);
    settings.compass_cal_x               = nvs_geti16(h, KEY_COMPASS_CAL_X, 0);
    settings.compass_cal_y               = nvs_geti16(h, KEY_COMPASS_CAL_Y, 0);
    settings.compass_cal_z               = nvs_geti16(h, KEY_COMPASS_CAL_Z, 0);
    settings.compass_calibrated          = nvs_getbool(h, KEY_COMPASS_CALED, false);
    settings.compass_declination_deg     = nvs_getfloat(h, KEY_DECL_DEG, 0.0f);
    settings.compass_declination_valid   = nvs_getbool(h, KEY_DECL_VALID, false);

    nvs_close_ns(h, false);
    return true;
}

// NOTE (CR-13): saveSettings() only covers the "bulk" fields below.
// beacon_sound, beacon_mac, beacon_found, compass_cal, declination, GPS state
// are written ONLY by their dedicated save*() functions and are NOT written here.
// Do NOT call saveSettings() expecting it to persist those fields.
bool saveSettings(const RadarSettings& settings) {
    if (!is_initialized) {
        Serial.println("[SETTINGS] ERROR: Not initialized, call init() first");
        return false;
    }

    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false);
    if (!h) {
        Serial.println("[SETTINGS] ERROR: Failed to open NVS for writing");
        return false;
    }

    nvs_set_u8(h, KEY_ZOOM_LEVEL, settings.default_zoom_level);
    nvs_setbool(h, KEY_GRID_VISIBLE, settings.grid_visible);
    nvs_set_u8(h, KEY_GRID_COLOR, settings.grid_color);
    nvs_set_u16(h, KEY_GPS_UPDATE, settings.gps_update_interval_ms);
    nvs_set_u16(h, KEY_GPS_LOG, settings.gps_log_interval_sec);
    nvs_setbool(h, KEY_GPS_LOG_EN, settings.gps_logging_enabled);
    nvs_setbool(h, KEY_WP_PERSIST, settings.waypoints_persistent);
    nvs_set_u8(h, KEY_WP_MAX, settings.max_visible_waypoints);
    nvs_set_u8(h, KEY_BRIGHTNESS, settings.brightness);
    nvs_setbool(h, KEY_AUTO_BRIGHT, settings.auto_brightness);
    nvs_set_u8(h, KEY_THEME, settings.theme);
    nvs_setbool(h, KEY_HUD_AUTO, settings.hud_auto_hide);
    nvs_set_u16(h, KEY_HUD_DELAY, settings.hud_auto_hide_seconds);
    nvs_setbool(h, KEY_DAYLIGHT, settings.daylight_mode);
    nvs_setbool(h, KEY_NORTH_IND, settings.north_indicator_enabled);
    nvs_set_u8(h, KEY_AUTO_SLEEP, settings.auto_sleep_timeout_minutes);
    nvs_setbool(h, KEY_HEADING_UP, settings.heading_up_mode);
    // wifi_enabled is session-only — not persisted to NVS, always boots as false
    // wifi_ap_enabled is persisted separately via saveWiFiAPEnabled(); skip here
    nvs_set_str(h, KEY_GPX_FILE, settings.active_gpx_file);
    nvs_setbool(h, KEY_GPX_AUTO, settings.gpx_auto_load);
    nvs_setbool(h, KEY_BTN_SOUND, settings.button_sound_enabled);
    nvs_setbool(h, KEY_DEV_MODE, settings.dev_mode);
    nvs_setbool(h, KEY_DEV_TAB, settings.dev_tab_visible);
    nvs_setbool(h, KEY_LOGGING_EN, settings.logging_enabled);

    nvs_close_ns(h, true);
    g_cached_settings = settings;
    Serial.println("[SETTINGS] Settings saved to NVS and cache updated");
    return true;
}

bool resetToDefaults() {
    if (!is_initialized) {
        Serial.println("[SETTINGS] ERROR: Not initialized, call init() first");
        return false;
    }
    RadarSettings defaults;
    getDefaultSettings(defaults);
    bool success = saveSettings(defaults);
    if (success) Serial.println("[SETTINGS] Settings reset to factory defaults");
    return success;
}

void printSettings() {
    const auto& s = getSettings();
    Serial.println("==== Radar Settings ====");
    Serial.printf("  Zoom Level: %d\n", s.default_zoom_level);
    Serial.printf("  Grid Visible: %s\n", s.grid_visible ? "Yes" : "No");
    Serial.printf("  GPS Update: %d ms\n", s.gps_update_interval_ms);
    Serial.printf("  GPS Logging: %s\n", s.gps_logging_enabled ? "Enabled" : "Disabled");
    Serial.printf("  Brightness: %d\n", s.brightness);
    Serial.printf("  WiFi Enabled: %s\n", s.wifi_enabled ? "Yes" : "No");
    Serial.printf("  WiFi AP Mode: %s\n", s.wifi_ap_enabled ? "Yes" : "No");
    Serial.printf("  Active GPX: %s\n", s.active_gpx_file[0] ? s.active_gpx_file : "(none)");
    Serial.println("========================");
}

// ============================================================================
// Individual setting save functions
// ============================================================================

#define NVS_SAVE_OPEN()  nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false); if (!h) return false;
#define NVS_SAVE_CLOSE() nvs_close_ns(h, true);

bool saveZoomLevel(uint8_t zoom_level) {
    NVS_SAVE_OPEN(); nvs_set_u8(h, KEY_ZOOM_LEVEL, zoom_level); NVS_SAVE_CLOSE();
    g_cached_settings.default_zoom_level = zoom_level; return true; }

bool saveGridVisibility(bool visible) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_GRID_VISIBLE, visible); NVS_SAVE_CLOSE();
    g_cached_settings.grid_visible = visible; return true; }

bool saveGridColor(uint8_t color) {
    NVS_SAVE_OPEN(); nvs_set_u8(h, KEY_GRID_COLOR, color); NVS_SAVE_CLOSE();
    g_cached_settings.grid_color = color; return true; }

bool saveGPSUpdateInterval(uint16_t interval_ms) {
    NVS_SAVE_OPEN(); nvs_set_u16(h, KEY_GPS_UPDATE, interval_ms); NVS_SAVE_CLOSE();
    g_cached_settings.gps_update_interval_ms = interval_ms; return true; }

bool saveGPSLogInterval(uint16_t interval_sec) {
    NVS_SAVE_OPEN(); nvs_set_u16(h, KEY_GPS_LOG, interval_sec); NVS_SAVE_CLOSE();
    g_cached_settings.gps_log_interval_sec = interval_sec; return true; }

bool saveGPSLoggingEnabled(bool enabled) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_GPS_LOG_EN, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.gps_logging_enabled = enabled; return true; }

bool saveWaypointsPersistent(bool persistent) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_WP_PERSIST, persistent); NVS_SAVE_CLOSE();
    g_cached_settings.waypoints_persistent = persistent; return true; }

bool saveMaxVisibleWaypoints(uint8_t max_waypoints) {
    NVS_SAVE_OPEN(); nvs_set_u8(h, KEY_WP_MAX, max_waypoints); NVS_SAVE_CLOSE();
    g_cached_settings.max_visible_waypoints = max_waypoints; return true; }

bool saveBrightness(uint8_t brightness) {
    NVS_SAVE_OPEN(); nvs_set_u8(h, KEY_BRIGHTNESS, brightness); NVS_SAVE_CLOSE();
    g_cached_settings.brightness = brightness; return true; }

bool saveAutoBrightness(bool enabled) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_AUTO_BRIGHT, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.auto_brightness = enabled; return true; }

bool saveTheme(uint8_t theme) {
    NVS_SAVE_OPEN(); nvs_set_u8(h, KEY_THEME, theme); NVS_SAVE_CLOSE();
    g_cached_settings.theme = theme; return true; }

bool saveHeadingUpMode(bool heading_up) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_HEADING_UP, heading_up); NVS_SAVE_CLOSE();
    g_cached_settings.heading_up_mode = heading_up;
    Serial.printf("[SETTINGS] Navigation mode: %s\n", heading_up ? "heading-up" : "north-up");
    return true; }

bool saveHUDAutoHide(bool enabled) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_HUD_AUTO, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.hud_auto_hide = enabled;
    Serial.printf("[SETTINGS] HUD auto-hide: %s\n", enabled ? "ON" : "OFF");
    return true; }

bool saveHUDAutoHideSeconds(uint16_t seconds) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_set_u16(h, KEY_HUD_DELAY, seconds); NVS_SAVE_CLOSE();
    g_cached_settings.hud_auto_hide_seconds = seconds;
    Serial.printf("[SETTINGS] HUD auto-hide delay: %us\n", (unsigned)seconds);
    return true; }

bool saveDaylightMode(bool enabled) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_DAYLIGHT, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.daylight_mode = enabled;
    Serial.printf("[SETTINGS] Daylight mode saved: %s\n", enabled ? "ON" : "OFF");
    return true; }

bool saveNorthIndicatorEnabled(bool enabled) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_NORTH_IND, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.north_indicator_enabled = enabled;
    Serial.printf("[SETTINGS] N indicator: %s\n", enabled ? "ON" : "OFF");
    return true; }

bool saveAutoSleepTimeout(uint8_t minutes) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_set_u8(h, KEY_AUTO_SLEEP, minutes); NVS_SAVE_CLOSE();
    g_cached_settings.auto_sleep_timeout_minutes = minutes;
    Serial.printf("[SETTINGS] Auto sleep: %s\n", minutes == 0 ? "OFF" : (String(minutes) + " min").c_str());
    return true; }

bool saveWiFiEnabled(bool enabled) {
    // Session-only — loadSettings() explicitly skips this field (always boots as false)
    // Writing to NVS would be a dead write; cache-only matches saveWiFiAPEnabled() pattern.
    g_cached_settings.wifi_enabled = enabled;
    Serial.printf("[SETTINGS] WiFi enabled (session): %s\n", enabled ? "ON" : "OFF");
    return true; }

bool saveWiFiAPEnabled(bool enabled) {
    g_cached_settings.wifi_ap_enabled = enabled;
    NVS_SAVE_OPEN();
    nvs_setbool(h, KEY_WIFI_AP_EN, enabled);
    NVS_SAVE_CLOSE();
    Serial.printf("[SETTINGS] WiFi AP mode: %s\n", enabled ? "ON (persisted)" : "OFF (persisted)");
    return true; }

bool saveWiFiSTABoot(bool enabled) {
    g_cached_settings.wifi_sta_boot = enabled;
    NVS_SAVE_OPEN();
    nvs_setbool(h, KEY_WIFI_STA_EN, enabled);
    NVS_SAVE_CLOSE();
    Serial.printf("[SETTINGS] WiFi STA boot: %s\n", enabled ? "ON (persisted)" : "OFF (persisted)");
    return true; }

bool saveAPSSID(const char* ssid) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) return false;
    strncpy(g_cached_settings.ap_ssid, ssid, sizeof(g_cached_settings.ap_ssid) - 1);
    g_cached_settings.ap_ssid[sizeof(g_cached_settings.ap_ssid) - 1] = '\0';
    NVS_SAVE_OPEN();
    nvs_set_str(h, KEY_AP_SSID, ssid);
    NVS_SAVE_CLOSE();
    Serial.printf("[SETTINGS] AP SSID updated: %s\n", ssid);
    return true; }

bool saveAPPassword(const char* pass) {
    if (!pass || strlen(pass) < 8 || strlen(pass) > 63) return false;
    strncpy(g_cached_settings.ap_password, pass, sizeof(g_cached_settings.ap_password) - 1);
    g_cached_settings.ap_password[sizeof(g_cached_settings.ap_password) - 1] = '\0';
    NVS_SAVE_OPEN();
    nvs_set_str(h, KEY_AP_PASS, pass);
    NVS_SAVE_CLOSE();
    Serial.println("[SETTINGS] AP password updated");
    return true; }

bool prepareForOTAReboot() {
    // Write stamp=0 + clear WiFi boot flags in one NVS transaction.
    // Writing stamp=0 guarantees a mismatch on next boot (FW_STAMP_VAL is always
    // a non-zero FNV-1a hash), so loadSettings() will always hit the mismatch
    // branch and clear the flags — even if the flag writes below somehow fail.
    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false);
    if (!h) {
        Serial.println("[SETTINGS] ERROR: prepareForOTAReboot — could not open NVS");
        return false;
    }
    nvs_set_u32(h, KEY_FW_STAMP, 0);
    nvs_setbool(h, KEY_WIFI_AP_EN, false);
    nvs_setbool(h, KEY_WIFI_STA_EN, false);
    nvs_close_ns(h, true);
    g_cached_settings.wifi_ap_enabled = false;
    g_cached_settings.wifi_sta_boot   = false;
    Serial.println("[SETTINGS] OTA reboot prepared — stamp invalidated, WiFi boot flags cleared");
    return true;
}

bool saveActiveGPXFile(const char* filepath) {
    NVS_SAVE_OPEN(); nvs_set_str(h, KEY_GPX_FILE, filepath); NVS_SAVE_CLOSE();
    strncpy(g_cached_settings.active_gpx_file, filepath, sizeof(g_cached_settings.active_gpx_file) - 1);
    g_cached_settings.active_gpx_file[sizeof(g_cached_settings.active_gpx_file) - 1] = '\0';
    return true; }

bool saveGPXAutoLoad(bool auto_load) {
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_GPX_AUTO, auto_load); NVS_SAVE_CLOSE();
    g_cached_settings.gpx_auto_load = auto_load; return true; }

// ============================================================================
// WiFi Credential Management
// ============================================================================

bool saveWiFi(const String& ssid, const String& password) {
    if (!is_initialized) {
        Serial.println("[SETTINGS] ERROR: Not initialized, call init() first");
        return false;
    }
    if (ssid.isEmpty() || ssid.length() > 32) {
        Serial.printf("[SETTINGS] ERROR: Invalid SSID length: %d\n", (int)ssid.length());
        return false;
    }
    if (password.length() > 64) {
        Serial.printf("[SETTINGS] ERROR: Invalid password length: %d\n", (int)password.length());
        return false;
    }

    nvs_handle_t h = nvs_open_ns(NAMESPACE_WIFI, false);
    if (!h) {
        Serial.println("[SETTINGS] ERROR: Failed to open WiFi namespace for writing");
        return false;
    }
    nvs_set_str(h, KEY_WIFI_SSID, ssid.c_str());
    nvs_set_str(h, KEY_WIFI_PASSWORD, password.c_str());
    nvs_close_ns(h, true);

    Serial.printf("[SETTINGS] WiFi credentials saved: SSID=\"%s\"\n", ssid.c_str());
    return true;
}

bool getWiFi(String& ssid, String& password) {
    if (!is_initialized) {
        ssid = ""; password = ""; return false;
    }

    nvs_handle_t h = nvs_open_ns(NAMESPACE_WIFI, true);
    if (!h) {
        Serial.println("[SETTINGS] WiFi credentials not found");
        ssid = ""; password = ""; return false;
    }

    char ssid_buf[33] = {};
    char pass_buf[65] = {};
    nvs_getstr(h, KEY_WIFI_SSID, ssid_buf, sizeof(ssid_buf));
    nvs_getstr(h, KEY_WIFI_PASSWORD, pass_buf, sizeof(pass_buf));
    nvs_close_ns(h, false);

    ssid = String(ssid_buf);
    password = String(pass_buf);

    if (ssid.isEmpty()) {
        Serial.println("[SETTINGS] No WiFi credentials saved");
        return false;
    }
    Serial.printf("[SETTINGS] WiFi credentials loaded: SSID=\"%s\"\n", ssid.c_str());
    return true;
}

bool hasWiFi() {
    if (!is_initialized) return false;
    nvs_handle_t h = nvs_open_ns(NAMESPACE_WIFI, true);
    if (!h) return false;
    char ssid_buf[33] = {};
    nvs_getstr(h, KEY_WIFI_SSID, ssid_buf, sizeof(ssid_buf));
    nvs_close_ns(h, false);
    return ssid_buf[0] != '\0';
}

bool clearWiFi() {
    if (!is_initialized) {
        Serial.println("[SETTINGS] ERROR: Not initialized");
        return false;
    }
    nvs_handle_t h = nvs_open_ns(NAMESPACE_WIFI, false);
    if (!h) {
        Serial.println("[SETTINGS] ERROR: Failed to open WiFi namespace");
        return false;
    }
    nvs_erase_all(h);
    nvs_close_ns(h, true);
    Serial.println("[SETTINGS] WiFi credentials cleared");
    return true;
}

bool saveButtonSoundEnabled(bool enabled) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_BTN_SOUND, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.button_sound_enabled = enabled;
    Serial.printf("[SETTINGS] Button sound: %s\n", enabled ? "ON" : "OFF");
    return true; }

bool saveDevMode(bool enabled) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_DEV_MODE, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.dev_mode = enabled;
    Serial.printf("[SETTINGS] Dev mode: %s\n", enabled ? "ON" : "OFF");
    return true; }

bool saveDevTabVisible(bool visible) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_DEV_TAB, visible); NVS_SAVE_CLOSE();
    g_cached_settings.dev_tab_visible = visible;
    Serial.printf("[SETTINGS] DEV tab visibility: %s\n", visible ? "ON" : "OFF");
    return true; }

bool saveLoggingEnabled(bool enabled) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_LOGGING_EN, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.logging_enabled = enabled;
    Serial.printf("[SETTINGS] System logging: %s\n", enabled ? "ON" : "OFF");
    return true; }

// ============================================================================
// GPS State Management (for Hot/Warm Start)
// ============================================================================

bool saveGPSState(double lat, double lon, uint32_t fix_time) {
    if (!is_initialized) { Serial.println("[SETTINGS] ERROR: Not initialized"); return false; }
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        Serial.printf("[SETTINGS] ERROR: Invalid GPS coordinates: %.6f, %.6f\n", lat, lon);
        return false;
    }

    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false);
    if (!h) { Serial.println("[SETTINGS] ERROR: Failed to open NVS for GPS state"); return false; }
    nvs_setdouble(h, KEY_GPS_LAST_LAT, lat);
    nvs_setdouble(h, KEY_GPS_LAST_LON, lon);
    nvs_set_u32(h, KEY_GPS_LAST_FIX, fix_time);
    nvs_setbool(h, KEY_GPS_HAS_POS, true);
    nvs_close_ns(h, true);

    g_cached_settings.gps_last_lat = lat;
    g_cached_settings.gps_last_lon = lon;
    g_cached_settings.gps_last_fix_time = fix_time;
    g_cached_settings.gps_has_saved_position = true;

    Serial.printf("[GPS_STATE] Position saved: %.6f, %.6f (time: %u)\n", lat, lon, (unsigned)fix_time);
    return true;
}

bool loadGPSState(double& lat, double& lon, uint32_t& fix_time) {
    if (!is_initialized) { Serial.println("[SETTINGS] ERROR: Not initialized"); return false; }

    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, true);
    if (!h) { Serial.println("[SETTINGS] ERROR: Failed to open NVS for GPS state read"); return false; }

    bool has_position = nvs_getbool(h, KEY_GPS_HAS_POS, false);
    if (!has_position) {
        nvs_close_ns(h, false);
        Serial.println("[GPS_STATE] No saved position found");
        return false;
    }

    lat      = nvs_getdouble(h, KEY_GPS_LAST_LAT, 0.0);
    lon      = nvs_getdouble(h, KEY_GPS_LAST_LON, 0.0);
    fix_time = nvs_getu32(h, KEY_GPS_LAST_FIX, 0);
    nvs_close_ns(h, false);

    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        Serial.println("[GPS_STATE] Invalid saved coordinates, ignoring");
        return false;
    }

    Serial.printf("[GPS_STATE] Position loaded: %.6f, %.6f (time: %u)\n", lat, lon, (unsigned)fix_time);
    return true;
}

// ============================================================================
// Beacon Proximity Settings
// ============================================================================

bool saveBeaconProximityEnabled(bool enabled) {
    // Session-only — update cache only, never write to NVS.
    // Loading from NVS was previously causing the feature to be blocked on boot.
    g_cached_settings.beacon_proximity_enabled = enabled;
    Serial.printf("[SETTINGS] Beacon proximity %s (session)\n", enabled ? "ENABLED" : "DISABLED");
    return true; }

bool saveBeaconSoundEnabled(bool enabled) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_BCN_SND, enabled); NVS_SAVE_CLOSE();
    g_cached_settings.beacon_sound_enabled = enabled;
    Serial.printf("[SETTINGS] Beacon sound %s\n", enabled ? "ENABLED" : "DISABLED");
    return true; }

bool saveBeaconMAC(const char* mac) {
    if (!is_initialized || !mac) return false;
    NVS_SAVE_OPEN(); nvs_set_str(h, KEY_BCN_MAC, mac); NVS_SAVE_CLOSE();
    strncpy(g_cached_settings.beacon_macs[0], mac, sizeof(g_cached_settings.beacon_macs[0]) - 1);
    g_cached_settings.beacon_macs[0][sizeof(g_cached_settings.beacon_macs[0]) - 1] = '\0';
    if (strlen(mac) > 0) g_cached_settings.beacon_count = 1;
    Serial.printf("[SETTINGS] Beacon MAC set: %s\n", mac);
    return true; }

bool saveBeaconMeasuredPower(int8_t power) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_set_i8(h, KEY_BCN_PWR, power); NVS_SAVE_CLOSE();
    g_cached_settings.beacon_measured_power = power;
    Serial.printf("[SETTINGS] Beacon measured power set: %d dBm\n", power);
    return true; }

bool saveBeaconPathLoss(float n) {
    if (!is_initialized) return false;
    if (n < 2.0f) n = 2.0f;
    if (n > 4.0f) n = 4.0f;
    NVS_SAVE_OPEN(); nvs_setfloat(h, KEY_BCN_N, n); NVS_SAVE_CLOSE();
    g_cached_settings.beacon_path_loss_n = n;
    Serial.printf("[SETTINGS] Beacon path loss exponent set: %.1f\n", n);
    return true; }

bool saveBeaconFound(bool found) {
    if (!is_initialized) return false;
    NVS_SAVE_OPEN(); nvs_setbool(h, KEY_BCN_FOUND, found); NVS_SAVE_CLOSE();
    g_cached_settings.beacon_found = found;
    Serial.printf("[SETTINGS] Beacon found state saved: %s\n", found ? "FOUND" : "MISSING");
    return true; }

bool saveCompassCalibration(int16_t x, int16_t y, int16_t z) {
    if (!is_initialized) return false;
    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false); if (!h) return false;
    nvs_set_i16(h, KEY_COMPASS_CAL_X, x);
    nvs_set_i16(h, KEY_COMPASS_CAL_Y, y);
    nvs_set_i16(h, KEY_COMPASS_CAL_Z, z);
    nvs_setbool(h, KEY_COMPASS_CALED, true);
    nvs_close_ns(h, true);
    g_cached_settings.compass_cal_x = x;
    g_cached_settings.compass_cal_y = y;
    g_cached_settings.compass_cal_z = z;
    g_cached_settings.compass_calibrated = true;
    Serial.printf("[SETTINGS] Compass calibration saved: X=%d Y=%d Z=%d\n", x, y, z);
    return true; }

bool saveDeclination(float declination_deg) {
    if (!is_initialized) return false;
    nvs_handle_t h = nvs_open_ns(NAMESPACE_SETTINGS, false); if (!h) return false;
    nvs_setfloat(h, KEY_DECL_DEG, declination_deg);
    nvs_setbool(h, KEY_DECL_VALID, true);
    nvs_close_ns(h, true);
    g_cached_settings.compass_declination_deg   = declination_deg;
    g_cached_settings.compass_declination_valid = true;
    Serial.printf("[SETTINGS] Magnetic declination saved: %.2f°\n", declination_deg);
    return true; }

} // namespace settings_manager
