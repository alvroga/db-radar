#include "standby_manager.h"
#include "hardware/display/backlight.h"
#include "hardware/connectivity/wifi_manager.h"
#include "hardware/connectivity/scanner.h"
#include "gpx/gpx_server.h"
#include "ui/navigation.h"
#include "ui/ui_manager.h"
#include "settings_manager.h"
#include "task_manager.h"
#include "system_logger.h"
#include "hardware/sensors/gps_bh880.h"
#include "hardware/connectivity/beacon_proximity.h"
#include "i2c_manager.h"
#include <lvgl.h>
#include <ctime>

namespace standby_manager {

// =============================================================================
// GLOBAL STATE
// =============================================================================

static StandbyState g_state = StandbyState::ACTIVE;
static StandbyStats g_stats = {0, 0, 0};

// Saved state before entering standby
static uint8_t g_saved_brightness = 100;
static bool g_wifi_was_enabled = false;
static bool g_ap_was_enabled = false;
static uint32_t g_standby_enter_time_ms = 0;
static uint32_t g_last_activity_ms = 0;  // Last user interaction (touch or button)

// Standby screen objects
static lv_obj_t* g_standby_screen = nullptr;
static lv_timer_t* g_standby_timer = nullptr;

// Configuration constants
constexpr uint32_t STANDBY_SCREEN_DURATION_MS = 3000;  // 3 seconds
constexpr uint32_t BACKLIGHT_FADE_DURATION_MS = 500;   // 500ms fade

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

static void createStandbyScreen();
static void destroyStandbyScreen();
static void standbyTimerCallback(lv_timer_t* timer);
static void applyStandbyPowerSettings();
static void restoreActivePowerSettings();

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bool init() {
    Serial.println("[STANDBY] Initializing standby manager...");

    g_state = StandbyState::ACTIVE;
    g_stats = {0, 0, 0};
    g_standby_screen = nullptr;
    g_standby_timer = nullptr;
    g_last_activity_ms = millis();  // Don't sleep immediately after boot

    Serial.println("[STANDBY] Standby manager initialized successfully");
    return true;
}

void enterStandby() {
    if (g_state != StandbyState::ACTIVE) {
        Serial.println("[STANDBY] Already in standby or transitioning - ignoring request");
        return;
    }

    Serial.println("[STANDBY] Entering standby mode...");
    system_logger::info("STANDBY", "State: ACTIVE -> ENTERING");
    g_state = StandbyState::ENTERING;
    g_standby_enter_time_ms = millis();

    // Save current state
    g_saved_brightness = backlight::getPercent();

    const auto& settings = settings_manager::getSettings();
    g_wifi_was_enabled = settings.wifi_enabled;
    g_ap_was_enabled = settings.wifi_ap_enabled;

    Serial.printf("[STANDBY] Saved state: brightness=%d%%, wifi=%s, ap=%s\n",
                  g_saved_brightness,
                  g_wifi_was_enabled ? "ON" : "OFF",
                  g_ap_was_enabled ? "ON" : "OFF");

    // Reset zoom to 100m immediately on sleep entry — ensures BLE is disabled
    // (BLE beacon scanning only runs at 50m zoom; resetting here guarantees it stops)
    ui_manager::getUIState().resetZoom();
    beacon_proximity::setEnabled(false);
    Serial.println("[STANDBY] Zoom reset to 100m, BLE beacon scanning disabled");

    // Create and show standby screen
    createStandbyScreen();

    // Start timer for transition to full standby after 2 seconds
    g_standby_timer = lv_timer_create(standbyTimerCallback, STANDBY_SCREEN_DURATION_MS, nullptr);
    lv_timer_set_repeat_count(g_standby_timer, 1);  // Fire once
}

void wakeFromStandby() {
    if (g_state != StandbyState::STANDBY) {
        Serial.println("[STANDBY] Not in standby - ignoring wake request");
        return;
    }

    Serial.println("[STANDBY] ========================================");
    Serial.println("[STANDBY] WAKING FROM STANDBY MODE");
    Serial.println("[STANDBY] ========================================");
    system_logger::info("STANDBY", "State: STANDBY -> WAKING (button press)");
    g_state = StandbyState::WAKING;

    // Calculate standby duration
    uint32_t duration_ms = millis() - g_standby_enter_time_ms;
    g_stats.last_standby_duration_ms = duration_ms;
    g_stats.total_standby_time_ms += duration_ms;

    Serial.printf("[STANDBY] Standby duration: %lu ms (%.1f minutes)\n",
                  duration_ms, duration_ms / 60000.0f);

    // Restore active power settings FIRST (backlight, WiFi, AP)
    Serial.println("[STANDBY] Restoring power settings...");
    restoreActivePowerSettings();

    // Small delay to let backlight stabilize
    delay(100);

    // Simply remove the overlay - underlying screen is still intact!
    Serial.println("[STANDBY] Removing standby overlay...");
    system_logger::info("STANDBY", "Destroying overlay, restoring screen");
    destroyStandbyScreen();

    // Reset zoom to default 100m on wake
    ui_manager::getUIState().resetZoom();

    // Always wake to the radar screen
    navigation::goToRadarScreen();

    // NOTE: Removed lv_timer_handler() call here - it was causing UI_Task freeze!
    // The UI Task will handle the display refresh automatically in its next loop iteration.
    // Calling lv_timer_handler() from here was unsafe because:
    // 1. This function is called from button callback (via UI update queue)
    // 2. The UI Task already calls lv_timer_handler() in its main loop
    // 3. Recursive/concurrent lv_timer_handler() calls corrupt LVGL state

    g_state = StandbyState::ACTIVE;
    system_logger::info("STANDBY", "State: WAKING -> ACTIVE");
    Serial.println("[STANDBY] ========================================");
    Serial.println("[STANDBY] WAKE COMPLETE - Original screen restored");
    Serial.println("[STANDBY] ========================================");
}

bool isStandby() {
    return g_state == StandbyState::STANDBY;
}

StandbyState getState() {
    return g_state;
}

StandbyStats getStats() {
    return g_stats;
}

const char* stateToString(StandbyState state) {
    switch (state) {
        case StandbyState::ACTIVE:   return "ACTIVE";
        case StandbyState::ENTERING: return "ENTERING";
        case StandbyState::STANDBY:  return "STANDBY";
        case StandbyState::WAKING:   return "WAKING";
        default:                     return "UNKNOWN";
    }
}

void notifyUserActivity() {
    g_last_activity_ms = millis();
}

void checkInactivityTimeout() {
    if (g_state != StandbyState::ACTIVE) return;

    const auto& settings = settings_manager::getSettings();
    uint8_t timeout_min = settings.auto_sleep_timeout_minutes;
    if (timeout_min == 0) return;  // Disabled

    uint32_t timeout_ms = (uint32_t)timeout_min * 60000UL;
    if (millis() - g_last_activity_ms >= timeout_ms) {
        Serial.printf("[STANDBY] Inactivity timeout (%u min) — entering auto-sleep\n", timeout_min);
        task_manager::queueUIUpdate({.type = task_manager::UIUpdateType::ENTER_STANDBY});
    }
}

// =============================================================================
// INTERNAL FUNCTIONS
// =============================================================================

static void createStandbyScreen() {
    Serial.println("[STANDBY] Creating standby screen overlay...");

    // Get current active screen (preserve it!)
    lv_obj_t* current_screen = lv_scr_act();
    if (!current_screen) {
        Serial.println("[STANDBY] ERROR: No active screen!");
        return;
    }

    // Create overlay on TOP of current screen (don't load new screen)
    // This preserves all existing screen objects
    g_standby_screen = lv_obj_create(current_screen);
    lv_obj_set_size(g_standby_screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_standby_screen, 0, 0);
    lv_obj_set_style_bg_color(g_standby_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_standby_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_standby_screen, 0, 0);
    lv_obj_set_style_radius(g_standby_screen, 0, 0);

    // "Standby Mode" title
    lv_obj_t* title = lv_label_create(g_standby_screen);
    lv_label_set_text(title, "STANDBY MODE");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_center(title);

    // Battery + Time info
    lv_obj_t* info = lv_label_create(g_standby_screen);

    // Get current time
    time_t now_time = time(nullptr);
    struct tm* timeinfo = localtime(&now_time);

    // Get battery status (using backlight as placeholder)
    uint8_t battery_percent = backlight::getPercent();

    char info_buf[64];
    snprintf(info_buf, sizeof(info_buf), "Battery: %d%% | Time: %02d:%02d",
             battery_percent,
             timeinfo->tm_hour,
             timeinfo->tm_min);
    lv_label_set_text(info, info_buf);
    lv_obj_set_style_text_color(info, lv_color_make(200, 200, 200), 0);
    lv_obj_align_to(info, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

    // Instructions
    lv_obj_t* inst = lv_label_create(g_standby_screen);
    lv_label_set_text(inst, "Press button to wake");
    lv_obj_set_style_text_color(inst, lv_color_make(128, 128, 128), 0);
    lv_obj_align(inst, LV_ALIGN_BOTTOM_MID, 0, -40);

    // Bring overlay to front (don't change screens!)
    lv_obj_move_foreground(g_standby_screen);

    Serial.println("[STANDBY] Standby overlay created (preserving underlying screen)");
}

static void destroyStandbyScreen() {
    if (g_standby_screen) {
        Serial.println("[STANDBY] Destroying standby screen...");
        lv_obj_del(g_standby_screen);
        g_standby_screen = nullptr;
    }
}

static void standbyTimerCallback(lv_timer_t* timer) {
    Serial.println("[STANDBY] Standby timer expired - entering full standby");
    system_logger::info("STANDBY", "Timer expired - applying power settings");

    // Apply standby power settings
    applyStandbyPowerSettings();

    // Update state
    g_state = StandbyState::STANDBY;
    g_stats.total_standby_count++;

    // CRITICAL: Do NOT call lv_timer_del() here. The timer was created with
    // repeat_count=1, so LVGL auto-deletes it after this callback returns.
    // Calling lv_timer_del() here causes a double-free → intermittent heap
    // corruption → random crashes on sleep entry.
    g_standby_timer = nullptr;  // Clear our reference; LVGL owns the deletion

    system_logger::info("STANDBY", "State: ENTERING -> STANDBY");
    Serial.printf("[STANDBY] Standby active (session #%u)\n", g_stats.total_standby_count);
}

static void applyStandbyPowerSettings() {
    Serial.println("[STANDBY] Applying standby power settings...");

    // Fade backlight to 0% over 500ms
    // Note: Smooth fade not implemented yet - instant off for now
    backlight::setPercent(0);
    Serial.println("[STANDBY] ✓ Display OFF (backlight 0%)");

    // Disable WiFi if it was enabled
    if (g_wifi_was_enabled) {
        scanner::setWiFiEnabled(false);
        Serial.println("[STANDBY] ✓ WiFi scanning disabled");
    }

    // Disable AP mode if it was enabled
    if (g_ap_was_enabled) {
        gpx_server::stop();
        wifi_manager::setEnabled(false);
        Serial.println("[STANDBY] ✓ AP mode disabled (WiFi OFF)");
    }

    // GPS: switch to Aggressive 1Hz to save power while maintaining fix
    // Note: M10 ignores interval mode (0x01) despite ACKing it - agg1 (0x02) is the only working low-power mode
    gps_bh880::setPowerMode(0x02);
    Serial.println("[STANDBY] ✓ GPS: Aggressive 1Hz mode (low power, maintains fix)");

    // Reduce CPU frequency to save ~85mA (240MHz → 80MHz)
    // Safe: GPS UART, I2C, and RGB panel have independent clock sources
    setCpuFrequencyMhz(80);
    Serial.println("[STANDBY] ✓ CPU: 80MHz (was 240MHz, saves ~85mA)");

    Serial.println("[STANDBY] Power settings applied - standby active");
    Serial.println("[STANDBY] Estimated power draw: ~106mA");
}

static void restoreActivePowerSettings() {
    Serial.println("[STANDBY] Restoring active power settings...");

    // Restore backlight
    backlight::setPercent(g_saved_brightness);
    Serial.printf("[STANDBY] ✓ Display ON (brightness %d%%)\n", g_saved_brightness);

    // Restore WiFi if it was enabled
    if (g_wifi_was_enabled) {
        scanner::setWiFiEnabled(true);
        Serial.println("[STANDBY] ✓ WiFi scanning restored");
    }

    // Restore AP mode if it was enabled
    if (g_ap_was_enabled) {
        wifi_manager::setEnabled(true);
        if (gpx_server::start()) {
            Serial.println("[STANDBY] ✓ AP mode restored (Radar-GPX)");
        } else {
            Serial.println("[STANDBY] ✗ Failed to restore AP mode");
        }
    }

    // GPS: restore full power
    gps_bh880::setPowerMode(0x00);
    Serial.println("[STANDBY] ✓ GPS: Full Power mode restored");

    // Restore CPU frequency to 240MHz
    setCpuFrequencyMhz(240);
    Serial.println("[STANDBY] ✓ CPU: 240MHz restored");

    // Recover I2C bus and CST820 touch controller.
    // After standby, the ESP32 I2C controller FSM can be stuck in error state —
    // resetBus() (9 SCL pulses) alone is not enough because it doesn't reset the
    // hardware state machine. Full reinit tears down and rebuilds the driver.
    i2c_manager::reinit();
    Serial.println("[STANDBY] ✓ I2C bus fully re-initialized");
    delay(20);

    // Toggle TP_RST (EXIO pin 1, active-low) to hard-reset the CST820.
    // Read current EXIO output state first so we preserve all other pin states.
    uint8_t exio_out = 0xFF;  // Safe default: all pins high (inactive)
    if (!i2c_manager::exio::readOutput(exio_out)) {
        Serial.println("[STANDBY] ⚠ EXIO read failed — using default 0xFF for TP_RST");
    }
    // Assert TP_RST low
    bool rst_ok = i2c_manager::exio::rawWrite(i2c_manager::exio::REG_OUTPUT,
                                              exio_out & ~(1u << (uint8_t)i2c_manager::exio::TP_RST));
    delay(15);
    // Deassert TP_RST high
    rst_ok &= i2c_manager::exio::rawWrite(i2c_manager::exio::REG_OUTPUT,
                                           exio_out | (1u << (uint8_t)i2c_manager::exio::TP_RST));
    delay(100);  // CST820 needs ~100ms to boot after reset

    if (rst_ok) {
        Serial.println("[STANDBY] ✓ CST820 TP_RST toggled");
    } else {
        Serial.println("[STANDBY] ⚠ CST820 TP_RST toggle failed (EXIO not responding)");
    }

    // Verify CST820 is responding before returning
    if (i2c_manager::ping(i2c_manager::TOUCH_DEVICE)) {
        Serial.println("[STANDBY] ✓ CST820 responding on I2C");
    } else {
        Serial.println("[STANDBY] ⚠ CST820 not responding — touch may be unavailable");
    }

    Serial.println("[STANDBY] Active power settings restored");
}

} // namespace standby_manager
