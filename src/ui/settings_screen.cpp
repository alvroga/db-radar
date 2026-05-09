#include "settings_screen.h"
#include "ui_manager.h"
#include "system_config.h"
#include "settings_manager.h"
#include "scanner.h"
#include "wifi_manager.h"
#include "backlight.h"
#include "gps_bh880.h"
#include "navigation.h"
#include "gpx_loader.h"
#include "gpx_server.h"
#include "ui/dev_screen.h"
#include "hardware/buzzer.h"
#include "core/device_manager.h"
#include "hardware/sensors/compass_qmc5883l.h"
#include "hardware/connectivity/beacon_proximity.h"
#include "core/arduino_compat.h"
#include "esp_wifi.h"
#include "esp_system.h"

// ============================================================================
// COMPASS CALIBRATION OVERLAY STATE
// ============================================================================
static lv_obj_t*   g_cal_overlay      = nullptr;
static lv_obj_t*   g_cal_heading_lbl  = nullptr;
static lv_obj_t*   g_cal_coverage_lbl = nullptr;
static lv_obj_t*   g_cal_status_lbl   = nullptr;
static lv_obj_t*   g_cal_save_btn     = nullptr;
static lv_timer_t* g_cal_timer        = nullptr;
static int16_t g_cal_min_x, g_cal_max_x, g_cal_min_y, g_cal_max_y;
static uint32_t g_cal_start_ms = 0;
static bool g_cal_good_coverage = false;

static constexpr uint32_t CAL_DURATION_MS  = 60000;  // 60-second rotation window
static constexpr int16_t  CAL_SPAN_OK      = 3000;   // Minimum span for OK coverage
static constexpr int16_t  CAL_SPAN_GOOD    = 5000;   // Span for GOOD coverage (needs full rotation)

static void calTimerCb(lv_timer_t*) {
    const CompassData& cd = device_manager::getDeviceState().last_compass_data;
    if (cd.last_update_ms == 0) return;

    // Track raw min/max for hard-iron offset computation
    if (cd.x_raw < g_cal_min_x) g_cal_min_x = cd.x_raw;
    if (cd.x_raw > g_cal_max_x) g_cal_max_x = cd.x_raw;
    if (cd.y_raw < g_cal_min_y) g_cal_min_y = cd.y_raw;
    if (cd.y_raw > g_cal_max_y) g_cal_max_y = cd.y_raw;

    int32_t span_x = (g_cal_max_x > g_cal_min_x) ? ((int32_t)g_cal_max_x - g_cal_min_x) : 0;
    int32_t span_y = (g_cal_max_y > g_cal_min_y) ? ((int32_t)g_cal_max_y - g_cal_min_y) : 0;

    // Live heading display
    char buf[48];
    if (!isnan(cd.heading)) {
        snprintf(buf, sizeof(buf), "%.1f deg", cd.heading);
    } else {
        snprintf(buf, sizeof(buf), "--- deg");
    }
    lv_label_set_text(g_cal_heading_lbl, buf);

    // Coverage display: spans show how much of the magnetic circle was captured
    const char* quality;
    lv_color_t qual_color;
    if (span_x >= CAL_SPAN_GOOD && span_y >= CAL_SPAN_GOOD) {
        quality = "GOOD";
        qual_color = lv_color_hex(0x00FF00);
    } else if (span_x >= CAL_SPAN_OK && span_y >= CAL_SPAN_OK) {
        quality = "OK";
        qual_color = lv_color_hex(0xFFAA00);
    } else {
        quality = "LOW";
        qual_color = lv_color_hex(0xFF4444);
    }
    snprintf(buf, sizeof(buf), "X:%d  Y:%d  [%s]", span_x, span_y, quality);
    lv_label_set_text(g_cal_coverage_lbl, buf);
    lv_obj_set_style_text_color(g_cal_coverage_lbl, qual_color, 0);

    // Unlock Save early once coverage is sufficient (both spans OK)
    bool sufficient = (span_x >= CAL_SPAN_OK && span_y >= CAL_SPAN_OK);
    if (sufficient && !g_cal_good_coverage) {
        g_cal_good_coverage = true;
        lv_obj_clear_state(g_cal_save_btn, LV_STATE_DISABLED);
    }

    uint32_t elapsed = millis() - g_cal_start_ms;
    if (elapsed >= CAL_DURATION_MS) {
        lv_label_set_text(g_cal_status_lbl, sufficient ? "Done! Tap Save." : "Done — coverage low, try again.");
        lv_timer_del(g_cal_timer);
        g_cal_timer = nullptr;
    } else {
        uint32_t remaining = (CAL_DURATION_MS - elapsed) / 1000 + 1;
        if (g_cal_good_coverage) {
            snprintf(buf, sizeof(buf), "Coverage OK! Save or keep rotating (%lus)", remaining);
        } else {
            snprintf(buf, sizeof(buf), "Keep rotating slowly... %lus", remaining);
        }
        lv_label_set_text(g_cal_status_lbl, buf);
    }
}

static void openCalibrationOverlay() {
    if (g_cal_overlay) return;  // Already open

    // Full-screen dark overlay
    g_cal_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_cal_overlay, 480, 480);
    lv_obj_set_pos(g_cal_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_cal_overlay, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(g_cal_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g_cal_overlay, 0, 0);
    lv_obj_clear_flag(g_cal_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(g_cal_overlay);
    lv_label_set_text(title, "Compass Calibration");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title, &iosevka_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // Instruction
    lv_obj_t* instr = lv_label_create(g_cal_overlay);
    lv_label_set_text(instr, "Flat on a surface. Rotate slowly 2 full circles.\nSave when coverage shows GOOD.");
    lv_obj_set_style_text_color(instr, lv_color_white(), 0);
    lv_obj_set_style_text_font(instr, &iosevka_16, 0);
    lv_label_set_long_mode(instr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(instr, 340);
    lv_obj_align(instr, LV_ALIGN_TOP_MID, 0, 65);

    // Live heading
    g_cal_heading_lbl = lv_label_create(g_cal_overlay);
    lv_label_set_text(g_cal_heading_lbl, "--- deg");
    lv_obj_set_style_text_color(g_cal_heading_lbl, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(g_cal_heading_lbl, &iosevka_16, 0);
    lv_obj_align(g_cal_heading_lbl, LV_ALIGN_CENTER, 0, -55);

    // Coverage indicator
    g_cal_coverage_lbl = lv_label_create(g_cal_overlay);
    lv_label_set_text(g_cal_coverage_lbl, "X:0  Y:0  [LOW]");
    lv_obj_set_style_text_color(g_cal_coverage_lbl, lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_text_font(g_cal_coverage_lbl, &iosevka_16, 0);
    lv_obj_align(g_cal_coverage_lbl, LV_ALIGN_CENTER, 0, -25);

    // Status
    g_cal_status_lbl = lv_label_create(g_cal_overlay);
    lv_label_set_text(g_cal_status_lbl, "Tap Start when ready.");
    lv_obj_set_style_text_color(g_cal_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(g_cal_status_lbl, &iosevka_16, 0);
    lv_label_set_long_mode(g_cal_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_cal_status_lbl, 320);
    lv_obj_align(g_cal_status_lbl, LV_ALIGN_CENTER, 0, 15);

    // Start button
    lv_obj_t* start_btn = lv_btn_create(g_cal_overlay);
    lv_obj_set_size(start_btn, 120, 45);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, -70, -60);
    lv_obj_set_style_bg_color(start_btn, lv_color_hex(0x004400), 0);
    lv_obj_t* start_lbl = lv_label_create(start_btn);
    lv_label_set_text(start_lbl, "Start");
    lv_obj_center(start_lbl);
    lv_obj_add_event_cb(start_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        // Init min/max trackers
        g_cal_min_x = INT16_MAX; g_cal_max_x = INT16_MIN;
        g_cal_min_y = INT16_MAX; g_cal_max_y = INT16_MIN;
        g_cal_start_ms = millis();
        g_cal_good_coverage = false;
        lv_obj_add_state(g_cal_save_btn, LV_STATE_DISABLED);
        lv_label_set_text(g_cal_status_lbl, "Keep rotating...");
        lv_obj_set_style_text_color(g_cal_status_lbl, lv_color_white(), 0);
        if (g_cal_timer) { lv_timer_del(g_cal_timer); }
        g_cal_timer = lv_timer_create(calTimerCb, 200, nullptr);
    }, LV_EVENT_CLICKED, nullptr);

    // Save button (disabled until calibration completes)
    g_cal_save_btn = lv_btn_create(g_cal_overlay);
    lv_obj_set_size(g_cal_save_btn, 120, 45);
    lv_obj_align(g_cal_save_btn, LV_ALIGN_BOTTOM_MID, 70, -60);
    lv_obj_set_style_bg_color(g_cal_save_btn, lv_color_hex(0x003366), 0);
    lv_obj_add_state(g_cal_save_btn, LV_STATE_DISABLED);
    lv_obj_t* save_lbl = lv_label_create(g_cal_save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(g_cal_save_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        int16_t ox = (g_cal_max_x + g_cal_min_x) / 2;
        int16_t oy = (g_cal_max_y + g_cal_min_y) / 2;
        compass_qmc5883l::setCalibration(ox, oy, 0);
        settings_manager::saveCompassCalibration(ox, oy, 0);
        Serial.printf("[CAL] Compass calibrated: X offset=%d, Y offset=%d\n", ox, oy);
        if (g_cal_timer) { lv_timer_del(g_cal_timer); g_cal_timer = nullptr; }
        lv_obj_del(g_cal_overlay);
        g_cal_overlay = nullptr;
    }, LV_EVENT_CLICKED, nullptr);

    // Cancel button
    lv_obj_t* cancel_btn = lv_btn_create(g_cal_overlay);
    lv_obj_set_size(cancel_btn, 100, 35);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x440000), 0);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        if (g_cal_timer) { lv_timer_del(g_cal_timer); g_cal_timer = nullptr; }
        lv_obj_del(g_cal_overlay);
        g_cal_overlay = nullptr;
    }, LV_EVENT_CLICKED, nullptr);
}

// Forward declarations for callbacks
static void showHelpModal(const char* title, const char* content);
static void onHelpModalClose(lv_event_t* e);
static void onRefreshWaypoints(lv_event_t* e);
static void updateWaypointCountLabel();

// Sound tab cross-switch references (needed for All Sounds ↔ individual sync)
static lv_obj_t* g_sound_all_sw       = nullptr;
static lv_obj_t* g_sound_proximity_sw = nullptr;
static lv_obj_t* g_sound_button_sw    = nullptr;
static bool      g_sound_updating     = false;  // Guard against callback loops

// WiFi AP mode status label (shows AP network info when active)
static lv_obj_t* g_wifi_ap_status_label = nullptr;

// WiFi/AP switch pointers (file-level for open() state sync)
static lv_obj_t* g_wifi_switch = nullptr;
static lv_obj_t* g_ap_switch   = nullptr;

// AP credential value labels (updated when user saves new SSID/password)
static lv_obj_t* g_ap_ssid_val_label = nullptr;
static lv_obj_t* g_ap_pass_val_label = nullptr;

// AP credential edit dialog state
static struct {
    lv_obj_t* overlay;
    lv_obj_t* textarea;
    lv_obj_t* keyboard;
    lv_obj_t* value_label;
    bool      is_ssid;
} s_ap_cred = {};

static void apCredSave(lv_event_t*) {
    const char* text = lv_textarea_get_text(s_ap_cred.textarea);
    if (s_ap_cred.is_ssid) {
        if (settings_manager::saveAPSSID(text)) {
            lv_label_set_text(s_ap_cred.value_label, text);
        }
    } else {
        if (settings_manager::saveAPPassword(text)) {
            lv_label_set_text(s_ap_cred.value_label, text);
        } else {
            // Show error (password too short)
            lv_label_set_text(s_ap_cred.value_label, "Min 8 chars");
        }
    }
    lv_obj_del(s_ap_cred.overlay);
    memset(&s_ap_cred, 0, sizeof(s_ap_cred));
}

static void apCredCancel(lv_event_t*) {
    lv_obj_del(s_ap_cred.overlay);
    memset(&s_ap_cred, 0, sizeof(s_ap_cred));
}

static void openAPCredDialog(bool is_ssid, lv_obj_t* value_label) {
    if (s_ap_cred.overlay) return;  // already open

    s_ap_cred.is_ssid     = is_ssid;
    s_ap_cred.value_label = value_label;

    s_ap_cred.overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ap_cred.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ap_cred.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ap_cred.overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_ap_cred.overlay, 0, 0);
    lv_obj_clear_flag(s_ap_cred.overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Card: textarea at top, keyboard below, buttons at bottom
    // Heights: 10 + 44 (textarea) + 8 + 175 (keyboard) + 8 + 44 (buttons) + 70 = 359
    lv_obj_t* card = lv_obj_create(s_ap_cred.overlay);
    lv_obj_set_size(card, 420, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00AA44), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Textarea at top — pre-filled with current value as both hint and editable content
    s_ap_cred.textarea = lv_textarea_create(card);
    lv_obj_set_size(s_ap_cred.textarea, 390, 44);
    lv_obj_align(s_ap_cred.textarea, LV_ALIGN_TOP_MID, 0, 10);
    lv_textarea_set_one_line(s_ap_cred.textarea, true);
    lv_obj_set_style_bg_color(s_ap_cred.textarea, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(s_ap_cred.textarea, lv_color_hex(0xFFFFFF), 0);
    lv_textarea_set_max_length(s_ap_cred.textarea, is_ssid ? 32 : 63);
    // Pre-fill with current value
    const auto& st = settings_manager::getSettings();
    lv_textarea_set_text(s_ap_cred.textarea, is_ssid ? st.ap_ssid : st.ap_password);

    // Keyboard directly below textarea
    s_ap_cred.keyboard = lv_keyboard_create(card);
    lv_obj_set_size(s_ap_cred.keyboard, 410, 175);
    lv_obj_align(s_ap_cred.keyboard, LV_ALIGN_TOP_MID, 0, 62);
    lv_keyboard_set_textarea(s_ap_cred.keyboard, s_ap_cred.textarea);
    lv_obj_set_style_bg_color(s_ap_cred.keyboard, lv_color_hex(0x1A1A1A), 0);

    // Buttons below keyboard
    lv_obj_t* btn_cancel = lv_btn_create(card);
    lv_obj_set_size(btn_cancel, 150, 40);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444455), 0);
    lv_obj_add_event_cb(btn_cancel, apCredCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, &iosevka_16, 0);
    lv_obj_center(lbl_cancel);

    lv_obj_t* btn_save = lv_btn_create(card);
    lv_obj_set_size(btn_save, 150, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x00AA44), 0);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x00CC55), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_save, apCredSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save");
    lv_obj_set_style_text_font(lbl_save, &iosevka_16, 0);
    lv_obj_center(lbl_save);
}

// GPS Help modal (for info/help dialogs)
static lv_obj_t* g_help_modal = nullptr;


// GPX waypoint count indicator (shows current/max waypoint count)
static lv_obj_t* g_waypoint_count_label = nullptr;

// Custom scrollable tab bar state (file-scope so captureless lambdas can access)
static lv_obj_t* g_custom_tab_btns[6] = {};
static lv_obj_t* g_custom_tabview     = nullptr;
static int        g_custom_tab_count  = 0;

namespace settings_screen {

static void createBeaconTab(lv_obj_t* parent);  // defined after createSoundTab

void create() {
    ui_manager::UIState& ui = ui_manager::getUIState();

    // Don't recreate if already exists
    if (ui.screen_settings) {
        Serial.println("[SETTINGS] Settings screen already exists");
        return;
    }

    Serial.println("[SETTINGS] Creating settings screen...");

    // Reset all static label pointers — previous screen was force-deleted, these are dangling
    g_wifi_ap_status_label = nullptr;
    g_wifi_switch          = nullptr;
    g_ap_switch            = nullptr;
    g_ap_ssid_val_label    = nullptr;
    g_ap_pass_val_label    = nullptr;
    memset(&s_ap_cred, 0, sizeof(s_ap_cred));
    g_sound_all_sw         = nullptr;
    g_sound_proximity_sw   = nullptr;
    g_sound_button_sw      = nullptr;
    g_help_modal           = nullptr;
    g_waypoint_count_label = nullptr;
    // Calibration overlay — also null any running timer
    if (g_cal_timer) { lv_timer_del(g_cal_timer); g_cal_timer = nullptr; }
    g_cal_overlay      = nullptr;
    g_cal_heading_lbl  = nullptr;
    g_cal_coverage_lbl = nullptr;
    g_cal_status_lbl   = nullptr;
    g_cal_save_btn     = nullptr;

    // Create main screen with dark background
    ui.screen_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui.screen_settings, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(ui.screen_settings, LV_OPA_COVER, 0);

    // Create header bar (inset for circular display)
    lv_obj_t* header = lv_obj_create(ui.screen_settings);
    lv_obj_set_size(header, 360, 50);  // Match tabview width
    lv_obj_set_pos(header, 60, 0);     // Align with tabview
    lv_obj_set_style_bg_color(header, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Title in header (centered)
    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, "Settings");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_title, &iosevka_16, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // Create tabview with hidden built-in tab bar (custom scrollable bar created below)
    ui.settings_tabview = lv_tabview_create(ui.screen_settings, LV_DIR_TOP, 0);
    lv_obj_set_pos(ui.settings_tabview, 60, 100);  // Below custom tab bar
    lv_obj_set_size(ui.settings_tabview, 360, 380);
    lv_obj_set_style_bg_color(ui.settings_tabview, lv_color_hex(0x2A2A2A), 0);

    // Create tabs
    Serial.println("[SETTINGS] Creating tabs...");
    ui.settings_tab_gps = lv_tabview_add_tab(ui.settings_tabview, "GPS");
    ui.settings_tab_wifi = lv_tabview_add_tab(ui.settings_tabview, "WiFi");
    ui.settings_tab_display = lv_tabview_add_tab(ui.settings_tabview, "Display");
    ui.settings_tab_sound = lv_tabview_add_tab(ui.settings_tabview, "Sound");
    ui.settings_tab_beacon = lv_tabview_add_tab(ui.settings_tabview, "Beacon");

    // Conditionally create DEV tab (only if dev_tab_visible is enabled)
    const auto& settings = settings_manager::getSettings();
    if (settings.dev_tab_visible) {
        ui.settings_tab_dev = lv_tabview_add_tab(ui.settings_tabview, "DEV");
        Serial.println("[SETTINGS] DEV tab created (dev_tab_visible = true)");
    } else {
        Serial.println("[SETTINGS] DEV tab not created (dev_tab_visible = false)");
    }

    // Style tabs
    lv_obj_set_style_bg_color(ui.settings_tab_gps, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_color(ui.settings_tab_wifi, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_color(ui.settings_tab_display, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_color(ui.settings_tab_sound, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_color(ui.settings_tab_beacon, lv_color_hex(0x1A1A1A), 0);
    if (ui.settings_tab_dev) {
        lv_obj_set_style_bg_color(ui.settings_tab_dev, lv_color_hex(0x1A1A1A), 0);
    }

    // Custom scrollable tab bar (replaces hidden built-in btnmatrix)
    // Each button is 80px → 5 tabs = 400px, 6 tabs = 480px in 360px container → scrollable
    memset(g_custom_tab_btns, 0, sizeof(g_custom_tab_btns));
    g_custom_tabview    = ui.settings_tabview;
    g_custom_tab_count  = ui.settings_tab_dev ? 6 : 5;

    lv_obj_t* custom_bar = lv_obj_create(ui.screen_settings);
    lv_obj_set_size(custom_bar, 360, 50);
    lv_obj_set_pos(custom_bar, 60, 50);
    lv_obj_set_style_bg_color(custom_bar, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(custom_bar, 0, 0);
    lv_obj_set_style_pad_all(custom_bar, 3, 0);
    lv_obj_set_style_pad_column(custom_bar, 2, 0);
    lv_obj_set_scroll_dir(custom_bar, LV_DIR_HOR);
    lv_obj_clear_flag(custom_bar, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_flex_flow(custom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(custom_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char* const k_tab_names[] = {"GPS", "WiFi", "Display", "Sound", "Beacon", "DEV"};
    for (int i = 0; i < g_custom_tab_count; i++) {
        lv_obj_t* btn = lv_btn_create(custom_bar);
        lv_obj_set_size(btn, 80, 44);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        bool is_active = (i == 0);
        lv_obj_set_style_bg_color(btn,
            is_active ? lv_color_hex(0x003300) : lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_border_color(btn,
            is_active ? lv_color_hex(0x00AA00) : lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(btn, 1, 0);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, k_tab_names[i]);
        lv_obj_set_style_text_font(lbl, &iosevka_16, 0);
        lv_obj_set_style_text_color(lbl,
            is_active ? lv_color_hex(0x00FF00) : lv_color_hex(0x00AA00), 0);
        lv_obj_center(lbl);

        g_custom_tab_btns[i] = btn;
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            lv_tabview_set_act(g_custom_tabview, (uint16_t)idx, LV_ANIM_ON);
            for (int j = 0; j < g_custom_tab_count; j++) {
                if (!g_custom_tab_btns[j]) continue;
                bool active = (j == idx);
                lv_obj_set_style_bg_color(g_custom_tab_btns[j],
                    active ? lv_color_hex(0x003300) : lv_color_hex(0x1A1A1A), 0);
                lv_obj_set_style_border_color(g_custom_tab_btns[j],
                    active ? lv_color_hex(0x00AA00) : lv_color_hex(0x444444), 0);
                lv_obj_t* item_lbl = lv_obj_get_child(g_custom_tab_btns[j], 0);
                if (item_lbl) lv_obj_set_style_text_color(item_lbl,
                    active ? lv_color_hex(0x00FF00) : lv_color_hex(0x00AA00), 0);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    Serial.printf("[SETTINGS] Custom tab bar created (%d tabs)\n", g_custom_tab_count);

    // Populate tabs with content
    createGPSTab(ui.settings_tab_gps);
    createWiFiTab(ui.settings_tab_wifi);
    createDisplayTab(ui.settings_tab_display);
    createSoundTab(ui.settings_tab_sound);
    createBeaconTab(ui.settings_tab_beacon);
    if (ui.settings_tab_dev) {
        dev_screen::createDevTab(ui.settings_tab_dev);
    }

    // Red X Close button (stationary for all tabs) - exact from cc-analog-clock
    lv_obj_t* btn_close = lv_btn_create(ui.screen_settings);
    lv_obj_set_size(btn_close, 40, 40);
    lv_obj_set_pos(btn_close, 370, 110);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xFF0000), 0);  // Red background
    lv_obj_set_style_border_width(btn_close, 0, 0);
    lv_obj_set_style_radius(btn_close, 10, 0);  // Rounded corners
    lv_obj_add_event_cb(btn_close, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            Serial.println("[SETTINGS] Close (X) - disabling radio, returning to radar");

            // Stop GPX server (AP or STA-via-WiFi)
            if (gpx_server::isRunning()) {
                gpx_server::stop();
                settings_manager::saveWiFiAPEnabled(false);
            }

            // Stop WiFi station mode (if enabled through wifi_manager)
            if (wifi_manager::isEnabled()) {
                wifi_manager::setEnabled(false);
                settings_manager::saveWiFiEnabled(false);
            }

            // Force-stop radio — AP mode starts WiFi without going through
            // wifi_manager, so isEnabled() is false but radio may still be on.
            // Calling esp_wifi_stop() here also prevents the AP→STA auto-connect
            // that fires when gpx_server::stop() switches the mode back to STA.
            esp_wifi_stop();

            // Navigate back — settings screen is kept alive (cached).
            // Toggle states are re-synced from runtime state in open().
            navigation::goToRadarScreen();
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(btn_close);  // Ensure it's on top

    lv_obj_t* lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "X");
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_close, &iosevka_16, 0);  // Using 14 since we don't have 20
    lv_obj_center(lbl_close);

    Serial.println("[SETTINGS] Close button created in main screen");
    Serial.println("[SETTINGS] Settings screen created successfully");
}

void open() {
    Serial.println("[SETTINGS] Opening settings screen...");

    ui_manager::UIState& ui = ui_manager::getUIState();

    // Create settings screen if it doesn't exist
    if (!ui.screen_settings) {
        create();
    }

    // Load settings screen
    lv_scr_load(ui.screen_settings);

    // Re-sync WiFi/AP toggle states from actual runtime state.
    // The screen is cached across opens, so toggles can be stale if WiFi
    // was disabled externally (e.g. via close-button auto-disable).
    if (g_wifi_switch) {
        bool wifi_on = wifi_manager::isEnabled();
        if (wifi_on) lv_obj_add_state(g_wifi_switch, LV_STATE_CHECKED);
        else          lv_obj_clear_state(g_wifi_switch, LV_STATE_CHECKED);
    }
    if (g_ap_switch) {
        bool ap_on = gpx_server::isRunning();
        if (ap_on) lv_obj_add_state(g_ap_switch, LV_STATE_CHECKED);
        else        lv_obj_clear_state(g_ap_switch, LV_STATE_CHECKED);
    }

    Serial.println("[SETTINGS] Settings screen loaded");
}

void close() {
    Serial.println("[SETTINGS] Closing settings screen...");

    // Stop GPX server/AP if running
    if (gpx_server::isRunning()) {
        gpx_server::stop();
    }
    // Unconditionally clear both radio NVS flags — radios are session-only, never persist past close
    settings_manager::saveWiFiAPEnabled(false);

    // Stop WiFi STA if enabled
    if (wifi_manager::isEnabled()) {
        wifi_manager::setEnabled(false);
    }
    settings_manager::saveWiFiEnabled(false);

    esp_wifi_stop();  // Idempotent safety net — stops radio regardless of path

    navigation::goToRadarScreen();
    Serial.println("[SETTINGS] Returned to radar screen");
}

// ============================================================================
// TAB IMPLEMENTATIONS
// ============================================================================

void createGPSTab(lv_obj_t* parent) {
    lv_obj_set_style_pad_all(parent, 15, 0);

    int y_offset = 0;

    // Constellations info (read-only - BH-880 has all enabled)
    lv_obj_t* gnss_info = lv_label_create(parent);
    lv_label_set_text(gnss_info, "Constellations: All enabled (BH-880)");
    lv_obj_set_style_text_color(gnss_info, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(gnss_info, &iosevka_16, 0);
    lv_obj_align(gnss_info, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 30;

    // GPS Restart section
    lv_obj_t* restart_label = lv_label_create(parent);
    lv_label_set_text(restart_label, "GPS Restart:");
    lv_obj_set_style_text_color(restart_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(restart_label, &iosevka_16, 0);
    lv_obj_align(restart_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    // Info icon for GPS Restart
    lv_obj_t* restart_help_btn = lv_btn_create(parent);
    lv_obj_set_size(restart_help_btn, 24, 24);
    lv_obj_align(restart_help_btn, LV_ALIGN_TOP_LEFT, 250, y_offset - 2);
    lv_obj_set_style_radius(restart_help_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(restart_help_btn, lv_color_hex(0x0080FF), 0);
    lv_obj_t* restart_help_label = lv_label_create(restart_help_btn);
    lv_label_set_text(restart_help_label, "?");
    lv_obj_set_style_text_color(restart_help_label, lv_color_white(), 0);
    lv_obj_center(restart_help_label);
    lv_obj_add_event_cb(restart_help_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            showHelpModal(
                "GPS Restart Modes",
                "Force GPS to reset with different data:\n\n"
                "• Hot Start (1-5 seconds)\n"
                "  Keeps: Position, time, satellite data\n"
                "  Use when: GPS off briefly, same location\n\n"
                "• Warm Start (30-60 seconds)\n"
                "  Keeps: Time, basic satellite data\n"
                "  Use when: Moved far (100+ km), off for hours\n\n"
                "• Cold Start (2-5 minutes)\n"
                "  Clears: Everything, full reset\n"
                "  Use when: GPS stuck at wrong location,\n"
                "           traveled internationally,\n"
                "           troubleshooting GPS issues\n\n"
                "Most common use: Cold start when GPS shows wrong position and won't update"
            );
        }
    }, LV_EVENT_CLICKED, nullptr);

    y_offset += 25;

    const char* restart_names[] = {"Hot", "Warm", "Cold"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_btn_create(parent);
        lv_obj_set_size(btn, 70, 35);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, i * 80, y_offset);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, restart_names[i]);
        lv_obj_center(label);

        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                lv_obj_t* btn = lv_event_get_target(e);
                lv_obj_t* label = lv_obj_get_child(btn, 0);
                const char* text = lv_label_get_text(label);

                bool success = false;
                if (strcmp(text, "Hot") == 0) {
                    success = gps_bh880::hotStart();
                } else if (strcmp(text, "Warm") == 0) {
                    success = gps_bh880::warmStart();
                } else if (strcmp(text, "Cold") == 0) {
                    success = gps_bh880::coldStart();
                }

                if (success) {
                    Serial.printf("[GPS] %s start initiated\n", text);
                } else {
                    Serial.printf("[GPS] Failed to perform %s start\n", text);
                }
            }
        }, LV_EVENT_CLICKED, nullptr);
    }
    y_offset += 50;

    // Factory Reset button
    lv_obj_t* factory_btn = lv_btn_create(parent);
    lv_obj_set_size(factory_btn, 150, 40);
    lv_obj_align(factory_btn, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_color(factory_btn, lv_color_hex(0xFF4444), LV_PART_MAIN);

    lv_obj_t* factory_label = lv_label_create(factory_btn);
    lv_label_set_text(factory_label, "Factory Reset");
    lv_obj_center(factory_label);

    lv_obj_add_event_cb(factory_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            Serial.println("[GPS] Factory reset requested");
            if (gps_bh880::factoryReset()) {
                Serial.println("[GPS] Factory reset complete - all settings cleared");
            } else {
                Serial.println("[GPS] Failed to perform factory reset");
            }
        }
    }, LV_EVENT_CLICKED, nullptr);

    y_offset += 60;

    // GPX Waypoints section
    lv_obj_t* waypoint_label = lv_label_create(parent);
    lv_label_set_text(waypoint_label, "GPX Waypoints:");
    lv_obj_set_style_text_color(waypoint_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(waypoint_label, &iosevka_16, 0);
    lv_obj_align(waypoint_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 25;

    // Waypoint count indicator (current/max with color coding)
    g_waypoint_count_label = lv_label_create(parent);
    lv_obj_set_style_text_font(g_waypoint_count_label, &iosevka_16, 0);
    lv_obj_align(g_waypoint_count_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
    updateWaypointCountLabel();  // Initialize with current count

    y_offset += 30;

    // Refresh Waypoints button
    lv_obj_t* btn_refresh = lv_btn_create(parent);
    lv_obj_set_size(btn_refresh, 200, 40);
    lv_obj_align(btn_refresh, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(0x0080FF), LV_PART_MAIN);

    lv_obj_t* refresh_label = lv_label_create(btn_refresh);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH " Refresh Waypoints");
    lv_obj_center(refresh_label);

    lv_obj_add_event_cb(btn_refresh, onRefreshWaypoints, LV_EVENT_CLICKED, nullptr);

    y_offset += 50;

    // Bottom padding spacer (100px) - allows scrolling bottom elements to middle of screen
    lv_obj_t* bottom_spacer = lv_obj_create(parent);
    lv_obj_set_size(bottom_spacer, 1, 100);  // 1px wide, 100px tall
    lv_obj_align(bottom_spacer, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);  // Transparent
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);  // No border
    lv_obj_clear_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);  // Not clickable

    Serial.println("[SETTINGS] GPS tab created with GPX waypoint controls and 100px bottom padding");
}

void createWiFiTab(lv_obj_t* parent) {
    Serial.println("[SETTINGS] Creating WiFi tab...");

    int y_pos = -10;

    // Title
    lv_obj_t* lbl_title = lv_label_create(parent);
    lv_label_set_text(lbl_title, "Connectivity");
    lv_obj_set_style_text_font(lbl_title, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_title, 0, y_pos);
    y_pos += 30;

    // Info text
    lv_obj_t* lbl_info = lv_label_create(parent);
    lv_label_set_text(lbl_info, "WiFi and AP modes require a restart.\nRadar and beacon are unavailable\nwhile in WiFi or AP mode.");
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_info, &iosevka_16, 0);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_info, 300);
    lv_obj_set_pos(lbl_info, 0, y_pos);
    y_pos += 75;

    const auto& settings = settings_manager::getSettings();

    // WiFi STA Mode toggle
    lv_obj_t* lbl_wifi_enable = lv_label_create(parent);
    lv_label_set_text(lbl_wifi_enable, "WiFi Mode:");
    lv_obj_set_style_text_color(lbl_wifi_enable, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_wifi_enable, &iosevka_16, 0);
    lv_obj_set_pos(lbl_wifi_enable, 0, y_pos);

    lv_obj_t* wifi_switch = lv_switch_create(parent);
    lv_obj_set_pos(wifi_switch, 180, y_pos);
    // Switch is always OFF in radar boot (wifi_sta_boot is never active in radar mode)
    g_wifi_switch = wifi_switch;

    lv_obj_add_event_cb(wifi_switch, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
        lv_obj_t* sw = lv_event_get_target(e);
        bool is_checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
        if (!is_checked) return;  // toggle OFF does nothing (no active WiFi to stop)

        // MUTUAL EXCLUSION: uncheck AP if it was on
        if (g_ap_switch && lv_obj_has_state(g_ap_switch, LV_STATE_CHECKED)) {
            lv_obj_clear_state(g_ap_switch, LV_STATE_CHECKED);
            settings_manager::saveWiFiAPEnabled(false);
        }

        // Show restart confirmation
        lv_obj_t* overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* card = lv_obj_create(overlay);
        lv_obj_set_size(card, 400, 260);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A2E), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x0088FF), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title = lv_label_create(card);
        lv_label_set_text(title, "Restart Required");
        lv_obj_set_style_text_font(title, &iosevka_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x0088FF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        lv_obj_t* msg = lv_label_create(card);
        lv_label_set_text(msg,
            "WiFi Mode requires a restart.\n\n"
            "Radar and beacon proximity will be\n"
            "unavailable until you tap\n"
            "'Reboot to Radar' on the WiFi screen.");
        lv_obj_set_style_text_font(msg, &iosevka_16, 0);
        lv_obj_set_style_text_color(msg, lv_color_white(), 0);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, 360);
        lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 52);

        lv_obj_t* btn_cancel = lv_btn_create(card);
        lv_obj_set_size(btn_cancel, 160, 46);
        lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 12, -12);
        lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444455), 0);
        lv_obj_add_event_cb(btn_cancel, [](lv_event_t* e) {
            lv_obj_t* ov = (lv_obj_t*)lv_event_get_user_data(e);
            lv_obj_del(ov);
            if (g_wifi_switch) lv_obj_clear_state(g_wifi_switch, LV_STATE_CHECKED);
        }, LV_EVENT_CLICKED, overlay);
        lv_obj_t* lbl_cancel = lv_label_create(btn_cancel);
        lv_label_set_text(lbl_cancel, "Cancel");
        lv_obj_set_style_text_font(lbl_cancel, &iosevka_16, 0);
        lv_obj_center(lbl_cancel);

        lv_obj_t* btn_restart = lv_btn_create(card);
        lv_obj_set_size(btn_restart, 160, 46);
        lv_obj_align(btn_restart, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
        lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0x0055CC), 0);
        lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0x0077EE), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn_restart, [](lv_event_t*) {
            Serial.println("[WIFI] User confirmed restart for WiFi mode");
            settings_manager::saveWiFiSTABoot(true);
            settings_manager::saveWiFiAPEnabled(false);
            esp_restart();
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl_restart = lv_label_create(btn_restart);
        lv_label_set_text(lbl_restart, "Restart Now");
        lv_obj_set_style_text_font(lbl_restart, &iosevka_16, 0);
        lv_obj_center(lbl_restart);

        return;  // Don't fall through
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_pos += 50;

    // WiFi AP Mode Enable/Disable Switch
    lv_obj_t* lbl_ap_enable = lv_label_create(parent);
    lv_label_set_text(lbl_ap_enable, "Access Point:");
    lv_obj_set_style_text_color(lbl_ap_enable, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_ap_enable, &iosevka_16, 0);
    lv_obj_set_pos(lbl_ap_enable, 0, y_pos);

    lv_obj_t* ap_switch = lv_switch_create(parent);
    lv_obj_set_pos(ap_switch, 180, y_pos);

    // Set AP switch state based on saved setting
    if (settings.wifi_ap_enabled) {
        lv_obj_add_state(ap_switch, LV_STATE_CHECKED);
    }

    // Update static pointer for mutual exclusion with WiFi switch
    g_ap_switch = ap_switch;

    // AP switch event handler
    lv_obj_add_event_cb(ap_switch, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* sw = lv_event_get_target(e);
            bool is_checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

            Serial.printf("[WIFI_AP] AP mode switch changed: %s\n", is_checked ? "ON" : "OFF");

            // MUTUAL EXCLUSION: If enabling AP, disable WiFi Station mode
            if (is_checked && g_wifi_switch) {
                if (lv_obj_has_state(g_wifi_switch, LV_STATE_CHECKED)) {
                    Serial.println("[WIFI_AP] ⚠️ Disabling WiFi Station mode (WiFi/AP mutually exclusive)");

                    // Disable WiFi manager (disconnects from network)
                    wifi_manager::setEnabled(false);

                    // Update WiFi switch UI state (without triggering its event)
                    lv_obj_clear_state(g_wifi_switch, LV_STATE_CHECKED);

                    // Save WiFi disabled state to NVS
                    settings_manager::saveWiFiEnabled(false);

                    // Update WiFi status label
                    settings_screen::updateWiFiStatus();
                }
            }

            if (is_checked) {
                // AP mode requires a device restart so the AP starts on a clean heap
                // (before NimBLE ever runs). Show a confirmation dialog first.
                lv_obj_t* overlay = lv_obj_create(lv_layer_top());
                lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
                lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
                lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

                // Centered card
                lv_obj_t* card = lv_obj_create(overlay);
                lv_obj_set_size(card, 400, 250);
                lv_obj_center(card);
                lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A2E), 0);
                lv_obj_set_style_border_color(card, lv_color_hex(0xFFAA00), 0);
                lv_obj_set_style_border_width(card, 2, 0);
                lv_obj_set_style_radius(card, 12, 0);
                lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t* title = lv_label_create(card);
                lv_label_set_text(title, "Restart Required");
                lv_obj_set_style_text_font(title, &iosevka_20, 0);
                lv_obj_set_style_text_color(title, lv_color_hex(0xFFAA00), 0);
                lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

                lv_obj_t* msg = lv_label_create(card);
                lv_label_set_text(msg,
                    "AP Upload Mode requires a restart.\n\n"
                    "Radar and beacon proximity will be\n"
                    "unavailable until you tap\n"
                    "'Reboot to Radar' on the upload screen.");
                lv_obj_set_style_text_font(msg, &iosevka_16, 0);
                lv_obj_set_style_text_color(msg, lv_color_white(), 0);
                lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(msg, 360);
                lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 52);

                // Cancel — dismiss overlay, uncheck the AP switch
                lv_obj_t* btn_cancel = lv_btn_create(card);
                lv_obj_set_size(btn_cancel, 160, 46);
                lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 12, -12);
                lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444455), 0);
                lv_obj_add_event_cb(btn_cancel, [](lv_event_t* e) {
                    lv_obj_t* ov = (lv_obj_t*)lv_event_get_user_data(e);
                    lv_obj_del(ov);  // safe to delete parent from child callback
                    if (g_ap_switch) lv_obj_clear_state(g_ap_switch, LV_STATE_CHECKED);
                }, LV_EVENT_CLICKED, overlay);
                lv_obj_t* lbl_cancel = lv_label_create(btn_cancel);
                lv_label_set_text(lbl_cancel, "Cancel");
                lv_obj_set_style_text_font(lbl_cancel, &iosevka_16, 0);
                lv_obj_center(lbl_cancel);

                // Restart Now — save NVS, restart device
                lv_obj_t* btn_restart = lv_btn_create(card);
                lv_obj_set_size(btn_restart, 160, 46);
                lv_obj_align(btn_restart, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
                lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0xFF6600), 0);
                lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0xFF8800), LV_STATE_PRESSED);
                lv_obj_add_event_cb(btn_restart, [](lv_event_t*) {
                    Serial.println("[WIFI_AP] User confirmed restart for AP mode");
                    settings_manager::saveWiFiAPEnabled(true);
                    settings_manager::saveWiFiEnabled(false);
                    esp_restart();
                }, LV_EVENT_CLICKED, nullptr);
                lv_obj_t* lbl_restart = lv_label_create(btn_restart);
                lv_label_set_text(lbl_restart, "Restart Now");
                lv_obj_set_style_text_font(lbl_restart, &iosevka_16, 0);
                lv_obj_center(lbl_restart);

                return;  // Don't fall through to saveWiFiAPEnabled below
            } else {
                // Disable AP mode
                Serial.println("[WIFI_AP] Stopping Access Point mode...");
                if (gpx_server::isRunning()) {
                    gpx_server::stop();
                    Serial.println("[WIFI_AP] ✓ AP and web server stopped");
                }
            }

            // Save to NVS for persistence
            settings_manager::saveWiFiAPEnabled(is_checked);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_pos += 50;

    // AP Network Name row
    lv_obj_t* lbl_ap_ssid = lv_label_create(parent);
    lv_label_set_text(lbl_ap_ssid, "AP Network:");
    lv_obj_set_style_text_color(lbl_ap_ssid, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_ap_ssid, &iosevka_16, 0);
    lv_obj_set_pos(lbl_ap_ssid, 0, y_pos);

    g_ap_ssid_val_label = lv_label_create(parent);
    lv_label_set_text(g_ap_ssid_val_label, settings.ap_ssid);
    lv_obj_set_style_text_color(g_ap_ssid_val_label, lv_color_hex(0xAAAAFF), 0);
    lv_obj_set_style_text_font(g_ap_ssid_val_label, &iosevka_16, 0);
    lv_obj_set_pos(g_ap_ssid_val_label, 130, y_pos);

    lv_obj_t* btn_edit_ssid = lv_btn_create(parent);
    lv_obj_set_size(btn_edit_ssid, 70, 30);
    lv_obj_set_pos(btn_edit_ssid, 250, y_pos - 4);
    lv_obj_set_style_bg_color(btn_edit_ssid, lv_color_hex(0x444455), 0);
    lv_obj_add_event_cb(btn_edit_ssid, [](lv_event_t*) {
        openAPCredDialog(true, g_ap_ssid_val_label);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_edit_ssid = lv_label_create(btn_edit_ssid);
    lv_label_set_text(lbl_edit_ssid, "Edit");
    lv_obj_set_style_text_font(lbl_edit_ssid, &iosevka_16, 0);
    lv_obj_center(lbl_edit_ssid);

    y_pos += 40;

    // AP Password row
    lv_obj_t* lbl_ap_pass = lv_label_create(parent);
    lv_label_set_text(lbl_ap_pass, "AP Password:");
    lv_obj_set_style_text_color(lbl_ap_pass, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_ap_pass, &iosevka_16, 0);
    lv_obj_set_pos(lbl_ap_pass, 0, y_pos);

    g_ap_pass_val_label = lv_label_create(parent);
    lv_label_set_text(g_ap_pass_val_label, settings.ap_password);
    lv_obj_set_style_text_color(g_ap_pass_val_label, lv_color_hex(0xAAAAFF), 0);
    lv_obj_set_style_text_font(g_ap_pass_val_label, &iosevka_16, 0);
    lv_obj_set_pos(g_ap_pass_val_label, 130, y_pos);

    lv_obj_t* btn_edit_pass = lv_btn_create(parent);
    lv_obj_set_size(btn_edit_pass, 70, 30);
    lv_obj_set_pos(btn_edit_pass, 250, y_pos - 4);
    lv_obj_set_style_bg_color(btn_edit_pass, lv_color_hex(0x444455), 0);
    lv_obj_add_event_cb(btn_edit_pass, [](lv_event_t*) {
        openAPCredDialog(false, g_ap_pass_val_label);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_edit_pass = lv_label_create(btn_edit_pass);
    lv_label_set_text(lbl_edit_pass, "Edit");
    lv_obj_set_style_text_font(lbl_edit_pass, &iosevka_16, 0);
    lv_obj_center(lbl_edit_pass);

    Serial.println("[SETTINGS] WiFi tab created");
}

void createDisplayTab(lv_obj_t* parent) {
    lv_obj_set_style_pad_all(parent, 15, 0);

    // Load settings from cache for UI initialization
    const auto& settings = settings_manager::getSettings();

    int y_offset = 0;

    lv_obj_t* brightness_label = lv_label_create(parent);
    lv_label_set_text(brightness_label, "Brightness:");
    lv_obj_set_style_text_color(brightness_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(brightness_label, &iosevka_16, 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
    y_offset += 25;

    // Brightness slider
    lv_obj_t* brightness_slider = lv_slider_create(parent);
    lv_slider_set_range(brightness_slider, 5, 100);  // Minimum 5% to prevent black screen
    lv_slider_set_value(brightness_slider, backlight::getPercent(), LV_ANIM_OFF);
    lv_obj_set_width(brightness_slider, 200);
    lv_obj_align(brightness_slider, LV_ALIGN_TOP_LEFT, 0, y_offset);

    // Double the size of the slider knob (default is ~10px, make it 20px)
    lv_obj_set_style_pad_left(brightness_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_pad_right(brightness_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_pad_top(brightness_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_pad_bottom(brightness_slider, 10, LV_PART_KNOB);

    // Brightness value label
    lv_obj_t* brightness_value = lv_label_create(parent);
    char brightness_text[16];
    snprintf(brightness_text, sizeof(brightness_text), "%d%%", backlight::getPercent());
    lv_label_set_text(brightness_value, brightness_text);
    lv_obj_set_style_text_color(brightness_value, lv_color_white(), 0);
    lv_obj_align(brightness_value, LV_ALIGN_TOP_LEFT, 210, y_offset);

    lv_obj_add_event_cb(brightness_slider, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* slider = lv_event_get_target(e);
            int32_t value = lv_slider_get_value(slider);

            // Enforce minimum 5% brightness
            if (value < 5) {
                value = 5;
                lv_slider_set_value(slider, value, LV_ANIM_OFF);
            }

            backlight::setPercent(value);

            // Update value label
            lv_obj_t* parent = lv_obj_get_parent(slider);
            for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); i++) {
                lv_obj_t* child = lv_obj_get_child(parent, i);
                if (lv_obj_check_type(child, &lv_label_class)) {
                    const char* text = lv_label_get_text(child);
                    if (text && strstr(text, "%")) {
                        char new_text[16];
                        snprintf(new_text, sizeof(new_text), "%d%%", (int)value);
                        lv_label_set_text(child, new_text);
                        break;
                    }
                }
            }

            // Save to NVS for persistence (convert 0-100% to 0-255 brightness)
            uint8_t brightness = (value * 255) / 100;
            settings_manager::saveBrightness(brightness);

            Serial.printf("[DISPLAY] Brightness set to %d%% (saved to NVS)\\n", (int)value);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 50;

    // Navigation Mode dropdown
    lv_obj_t* nav_mode_label = lv_label_create(parent);
    lv_label_set_text(nav_mode_label, "Navigation Mode:");
    lv_obj_set_style_text_color(nav_mode_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(nav_mode_label, &iosevka_16, 0);
    lv_obj_align(nav_mode_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* nav_mode_dropdown = lv_dropdown_create(parent);
    lv_dropdown_set_options(nav_mode_dropdown, "Heading-Up\nNorth-Up");
    lv_obj_set_width(nav_mode_dropdown, 200);
    lv_obj_align(nav_mode_dropdown, LV_ALIGN_TOP_LEFT, 180, y_offset);

    // Load current setting from cache
    lv_dropdown_set_selected(nav_mode_dropdown, settings_manager::getSettings().heading_up_mode ? 0 : 1);

    // Event handler for navigation mode changes
    lv_obj_add_event_cb(nav_mode_dropdown, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* dropdown = lv_event_get_target(e);
            uint16_t selected = lv_dropdown_get_selected(dropdown);

            // Update UIState immediately
            ui_manager::UIState& ui = ui_manager::getUIState();
            ui.heading_up_mode = (selected == 0);  // 0=Heading-Up, 1=North-Up

            settings_manager::saveHeadingUpMode(ui.heading_up_mode);

            // Update radar display to reflect new mode
            navigation::updateRadarDisplay();

            const char* mode_name = ui.heading_up_mode ? "Heading-Up" : "North-Up";
            Serial.printf("[DISPLAY] Navigation mode set to %s (saved to NVS)\n", mode_name);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 50;

    // HUD Auto-Hide toggle switch
    lv_obj_t* hud_auto_hide_label = lv_label_create(parent);
    lv_label_set_text(hud_auto_hide_label, "HUD Auto-Hide:");
    lv_obj_set_style_text_color(hud_auto_hide_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(hud_auto_hide_label, &iosevka_16, 0);
    lv_obj_align(hud_auto_hide_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* hud_auto_hide_switch = lv_switch_create(parent);
    lv_obj_align(hud_auto_hide_switch, LV_ALIGN_TOP_LEFT, 180, y_offset);

    // Load current setting from NVS
    if (settings.hud_auto_hide) {
        lv_obj_add_state(hud_auto_hide_switch, LV_STATE_CHECKED);
    }

    // Event handler for HUD auto-hide toggle
    lv_obj_add_event_cb(hud_auto_hide_switch, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* sw = lv_event_get_target(e);
            bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

            settings_manager::saveHUDAutoHide(enabled);

            Serial.printf("[DISPLAY] HUD Auto-Hide %s (saved to NVS)\n", enabled ? "enabled" : "disabled");

            // Reload cached settings
            ui_manager::reloadHUDSettings();

            // If enabled, reset the interaction timer to start fresh
            if (enabled) {
                ui_manager::onScreenTouch();
            } else {
                // If disabled, ensure HUD is visible
                ui_manager::showHUD();
            }
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 50;

    // HUD Auto-Hide Duration dropdown
    lv_obj_t* hud_duration_label = lv_label_create(parent);
    lv_label_set_text(hud_duration_label, "Auto-Hide Delay:");
    lv_obj_set_style_text_color(hud_duration_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(hud_duration_label, &iosevka_16, 0);
    lv_obj_align(hud_duration_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* hud_duration_dropdown = lv_dropdown_create(parent);
    lv_dropdown_set_options(hud_duration_dropdown, "5 seconds\n10 seconds\n20 seconds\n30 seconds\n1 minute\nNever");
    lv_obj_set_width(hud_duration_dropdown, 200);
    lv_obj_align(hud_duration_dropdown, LV_ALIGN_TOP_LEFT, 180, y_offset);

    // Map current setting to dropdown index
    uint16_t duration_index = 1; // Default to 10 seconds
    switch (settings.hud_auto_hide_seconds) {
        case 5:   duration_index = 0; break;
        case 10:  duration_index = 1; break;
        case 20:  duration_index = 2; break;
        case 30:  duration_index = 3; break;
        case 60:  duration_index = 4; break;
        case 0:   duration_index = 5; break; // Never
        default:  duration_index = 1; break; // Default to 10 seconds
    }
    lv_dropdown_set_selected(hud_duration_dropdown, duration_index);

    // Event handler for duration changes
    lv_obj_add_event_cb(hud_duration_dropdown, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* dropdown = lv_event_get_target(e);
            uint16_t selected = lv_dropdown_get_selected(dropdown);

            // Map dropdown index to seconds
            uint16_t seconds = 10; // Default
            switch (selected) {
                case 0: seconds = 5; break;
                case 1: seconds = 10; break;
                case 2: seconds = 20; break;
                case 3: seconds = 30; break;
                case 4: seconds = 60; break;
                case 5: seconds = 0; break; // Never
            }

            settings_manager::saveHUDAutoHideSeconds(seconds);

            const char* duration_name = (seconds == 0) ? "Never" :
                                       (seconds == 60) ? "1 minute" :
                                       String(seconds).c_str();
            Serial.printf("[DISPLAY] HUD Auto-Hide delay set to %s (saved to NVS)\n", duration_name);

            // Reload cached settings
            ui_manager::reloadHUDSettings();

            // Reset interaction timer
            ui_manager::onScreenTouch();
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 50;

    // ========== DAYLIGHT MODE SECTION ==========
    // High contrast colors for outdoor visibility when sunlight overpowers the backlight
    lv_obj_t* daylight_mode_label = lv_label_create(parent);
    lv_label_set_text(daylight_mode_label, "Daylight Mode:");
    lv_obj_set_style_text_color(daylight_mode_label, lv_color_hex(0xFFFF00), 0);  // Yellow - important setting
    lv_obj_set_style_text_font(daylight_mode_label, &iosevka_16, 0);
    lv_obj_align(daylight_mode_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* daylight_mode_switch = lv_switch_create(parent);
    lv_obj_align(daylight_mode_switch, LV_ALIGN_TOP_LEFT, 180, y_offset);

    // Load current setting from NVS
    if (settings.daylight_mode) {
        lv_obj_add_state(daylight_mode_switch, LV_STATE_CHECKED);
    }

    // Event handler for daylight mode toggle
    lv_obj_add_event_cb(daylight_mode_switch, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* sw = lv_event_get_target(e);
            bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

            // Save to NVS
            settings_manager::saveDaylightMode(enabled);

            Serial.printf("[DISPLAY] Daylight Mode %s - %s\n",
                         enabled ? "ENABLED" : "DISABLED",
                         enabled ? "Using high contrast colors for outdoor visibility"
                                : "Using normal colors for indoor use");

            // Force radar display refresh to apply new colors
            // The color scheme will be updated on next updateRadarDisplay() call
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 30;

    // Daylight mode description
    lv_obj_t* daylight_desc = lv_label_create(parent);
    lv_label_set_text(daylight_desc, "Use bright background for outdoor visibility");
    lv_obj_set_style_text_color(daylight_desc, lv_color_hex(0x888888), 0);  // Gray
    lv_obj_set_style_text_font(daylight_desc, &iosevka_16, 0);
    lv_obj_align(daylight_desc, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 50;

    // ========== NORTH INDICATOR ==========
    lv_obj_t* north_ind_label = lv_label_create(parent);
    lv_label_set_text(north_ind_label, "N Indicator:");
    lv_obj_set_style_text_color(north_ind_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(north_ind_label, &iosevka_16, 0);
    lv_obj_align(north_ind_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* north_ind_switch = lv_switch_create(parent);
    lv_obj_align(north_ind_switch, LV_ALIGN_TOP_LEFT, 180, y_offset);

    if (settings_manager::getSettings().north_indicator_enabled) {
        lv_obj_add_state(north_ind_switch, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(north_ind_switch, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
            settings_manager::saveNorthIndicatorEnabled(enabled);
            navigation::updateRadarDisplay();
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 50;

    // ========== AUTO SLEEP SECTION ==========
    lv_obj_t* sleep_label = lv_label_create(parent);
    lv_label_set_text(sleep_label, "Auto Sleep:");
    lv_obj_set_style_text_color(sleep_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(sleep_label, &iosevka_16, 0);
    lv_obj_align(sleep_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* sleep_dropdown = lv_dropdown_create(parent);
    lv_dropdown_set_options(sleep_dropdown, "Off\n5 minutes\n10 minutes\n15 minutes\n30 minutes");
    lv_obj_set_width(sleep_dropdown, 200);
    lv_obj_align(sleep_dropdown, LV_ALIGN_TOP_LEFT, 180, y_offset);

    // Map current setting to dropdown index
    uint8_t sleep_index = 0;
    switch (settings.auto_sleep_timeout_minutes) {
        case 5:  sleep_index = 1; break;
        case 10: sleep_index = 2; break;
        case 15: sleep_index = 3; break;
        case 30: sleep_index = 4; break;
        default: sleep_index = 0; break;  // Off
    }
    lv_dropdown_set_selected(sleep_dropdown, sleep_index);

    lv_obj_add_event_cb(sleep_dropdown, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_obj_t* dropdown = lv_event_get_target(e);
            uint16_t selected = lv_dropdown_get_selected(dropdown);
            uint8_t minutes = 0;
            switch (selected) {
                case 1: minutes = 5;  break;
                case 2: minutes = 10; break;
                case 3: minutes = 15; break;
                case 4: minutes = 30; break;
                default: minutes = 0; break;  // Off
            }
            settings_manager::saveAutoSleepTimeout(minutes);
            Serial.printf("[DISPLAY] Auto sleep: %s\n", minutes == 0 ? "OFF" : (String(minutes) + " min").c_str());
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    y_offset += 30;

    lv_obj_t* sleep_desc = lv_label_create(parent);
    lv_label_set_text(sleep_desc, "Sleep after no touch/button activity");
    lv_obj_set_style_text_color(sleep_desc, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(sleep_desc, &iosevka_16, 0);
    lv_obj_align(sleep_desc, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 50;

    // Compass Calibration button
    lv_obj_t* cal_divider = lv_label_create(parent);
    lv_label_set_text(cal_divider, "Compass:");
    lv_obj_set_style_text_color(cal_divider, lv_color_white(), 0);
    lv_obj_set_style_text_font(cal_divider, &iosevka_16, 0);
    lv_obj_align(cal_divider, LV_ALIGN_TOP_LEFT, 0, y_offset);
    y_offset += 30;

    // Show calibration status
    const auto& cal_settings = settings_manager::getSettings();
    lv_obj_t* cal_status = lv_label_create(parent);
    lv_label_set_text(cal_status, cal_settings.compass_calibrated ? "Status: Calibrated" : "Status: Not calibrated");
    lv_obj_set_style_text_color(cal_status, cal_settings.compass_calibrated ?
                                 lv_color_hex(0x00AA00) : lv_color_hex(0xAA6600), 0);
    lv_obj_set_style_text_font(cal_status, &iosevka_16, 0);
    lv_obj_align(cal_status, LV_ALIGN_TOP_LEFT, 0, y_offset);
    y_offset += 30;

    lv_obj_t* cal_btn = lv_btn_create(parent);
    lv_obj_set_size(cal_btn, 200, 40);
    lv_obj_align(cal_btn, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_color(cal_btn, lv_color_hex(0x003366), 0);
    lv_obj_t* cal_btn_lbl = lv_label_create(cal_btn);
    lv_label_set_text(cal_btn_lbl, "Calibrate Compass");
    lv_obj_center(cal_btn_lbl);
    lv_obj_add_event_cb(cal_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            openCalibrationOverlay();
        }
    }, LV_EVENT_CLICKED, nullptr);
    y_offset += 55;

    // Bottom padding spacer (100px) - allows scrolling bottom elements to middle of screen
    lv_obj_t* bottom_spacer = lv_obj_create(parent);
    lv_obj_set_size(bottom_spacer, 1, 100);  // 1px wide, 100px tall
    lv_obj_align(bottom_spacer, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);  // Transparent
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);  // No border
    lv_obj_clear_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);  // Not clickable

    Serial.println("[SETTINGS] Display tab created with Compass Calibration button");
}

// ============================================================================
// SOUND TAB
// ============================================================================
void createSoundTab(lv_obj_t* parent) {
    lv_obj_set_style_pad_all(parent, 15, 0);

    int y_offset = 0;

    const auto& settings = settings_manager::getSettings();

    // ── header ──────────────────────────────────────────────────────────────
    lv_obj_t* section_label = lv_label_create(parent);
    lv_label_set_text(section_label, "SOUNDS");
    lv_obj_set_style_text_color(section_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(section_label, &iosevka_16, 0);
    lv_obj_align(section_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
    y_offset += 40;

    // ── All Sounds ───────────────────────────────────────────────────────────
    lv_obj_t* all_lbl = lv_label_create(parent);
    lv_label_set_text(all_lbl, "All Sounds:");
    lv_obj_set_style_text_color(all_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(all_lbl, &iosevka_16, 0);
    lv_obj_align(all_lbl, LV_ALIGN_TOP_LEFT, 0, y_offset);

    g_sound_all_sw = lv_switch_create(parent);
    lv_obj_align(g_sound_all_sw, LV_ALIGN_TOP_LEFT, 180, y_offset);
    if (settings.button_sound_enabled && settings.beacon_sound_enabled) {
        lv_obj_add_state(g_sound_all_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(g_sound_all_sw, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
        bool on = lv_obj_has_state(g_sound_all_sw, LV_STATE_CHECKED);
        g_sound_updating = true;
        if (on) {
            lv_obj_add_state(g_sound_proximity_sw, LV_STATE_CHECKED);
            lv_obj_add_state(g_sound_button_sw,    LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_sound_proximity_sw, LV_STATE_CHECKED);
            lv_obj_clear_state(g_sound_button_sw,    LV_STATE_CHECKED);
        }
        g_sound_updating = false;
        settings_manager::saveBeaconSoundEnabled(on);
        settings_manager::saveButtonSoundEnabled(on);
        buzzer::reloadSettings();
        if (on) buzzer::chirp(50);
        Serial.printf("[SOUND] All sounds %s\n", on ? "ENABLED" : "DISABLED");
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    y_offset += 50;

    // ── Proximity Sound ──────────────────────────────────────────────────────
    lv_obj_t* prox_lbl = lv_label_create(parent);
    lv_label_set_text(prox_lbl, "  Proximity Sound:");
    lv_obj_set_style_text_color(prox_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(prox_lbl, &iosevka_16, 0);
    lv_obj_align(prox_lbl, LV_ALIGN_TOP_LEFT, 0, y_offset);

    g_sound_proximity_sw = lv_switch_create(parent);
    lv_obj_align(g_sound_proximity_sw, LV_ALIGN_TOP_LEFT, 180, y_offset);
    if (settings.beacon_sound_enabled) {
        lv_obj_add_state(g_sound_proximity_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(g_sound_proximity_sw, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
        if (g_sound_updating) return;
        bool on = lv_obj_has_state(g_sound_proximity_sw, LV_STATE_CHECKED);
        bool btn_on = g_sound_button_sw && lv_obj_has_state(g_sound_button_sw, LV_STATE_CHECKED);
        if (on && btn_on)
            lv_obj_add_state(g_sound_all_sw, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(g_sound_all_sw, LV_STATE_CHECKED);
        settings_manager::saveBeaconSoundEnabled(on);
        Serial.printf("[SOUND] Proximity sound %s\n", on ? "ENABLED" : "DISABLED");
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    y_offset += 50;

    // ── Button Sound ─────────────────────────────────────────────────────────
    lv_obj_t* btn_lbl = lv_label_create(parent);
    lv_label_set_text(btn_lbl, "  Button Sound:");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(btn_lbl, &iosevka_16, 0);
    lv_obj_align(btn_lbl, LV_ALIGN_TOP_LEFT, 0, y_offset);

    g_sound_button_sw = lv_switch_create(parent);
    lv_obj_align(g_sound_button_sw, LV_ALIGN_TOP_LEFT, 180, y_offset);
    if (settings.button_sound_enabled) {
        lv_obj_add_state(g_sound_button_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(g_sound_button_sw, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
        if (g_sound_updating) return;
        bool on = lv_obj_has_state(g_sound_button_sw, LV_STATE_CHECKED);
        bool prox_on = g_sound_proximity_sw && lv_obj_has_state(g_sound_proximity_sw, LV_STATE_CHECKED);
        if (on && prox_on)
            lv_obj_add_state(g_sound_all_sw, LV_STATE_CHECKED);
        else
            lv_obj_clear_state(g_sound_all_sw, LV_STATE_CHECKED);
        settings_manager::saveButtonSoundEnabled(on);
        buzzer::reloadSettings();
        if (on) buzzer::chirp(50);
        Serial.printf("[SOUND] Button sound %s\n", on ? "ENABLED" : "DISABLED");
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    y_offset += 60;

    // ── Test Beep ────────────────────────────────────────────────────────────
    lv_obj_t* test_btn = lv_btn_create(parent);
    lv_obj_set_size(test_btn, 150, 40);
    lv_obj_align(test_btn, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_color(test_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(test_btn, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(test_btn, 2, 0);
    lv_obj_t* test_btn_lbl = lv_label_create(test_btn);
    lv_label_set_text(test_btn_lbl, "Test Beep");
    lv_obj_set_style_text_color(test_btn_lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(test_btn_lbl, &iosevka_16, 0);
    lv_obj_center(test_btn_lbl);
    lv_obj_add_event_cb(test_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            buzzer::beep(100);
        }
    }, LV_EVENT_CLICKED, nullptr);
    y_offset += 60;

    // ── Bottom spacer ─────────────────────────────────────────────────────────
    lv_obj_t* bottom_spacer = lv_obj_create(parent);
    lv_obj_set_size(bottom_spacer, 1, 100);
    lv_obj_align(bottom_spacer, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);
    lv_obj_clear_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);

    Serial.println("[SETTINGS] Sound tab created");
}

// ============================================================================
// BEACON TAB
// ============================================================================
static void createBeaconTab(lv_obj_t* parent) {
    lv_obj_set_style_pad_all(parent, 15, 0);

    const auto& settings = settings_manager::getSettings();
    int y_offset = 0;

    // Header
    lv_obj_t* hdr = lv_label_create(parent);
    lv_label_set_text(hdr, "BEACON");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(hdr, &iosevka_16, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, y_offset);
    y_offset += 40;

    // MAC address row
    lv_obj_t* mac_lbl = lv_label_create(parent);
    lv_label_set_text(mac_lbl, "MAC:");
    lv_obj_set_style_text_color(mac_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(mac_lbl, &iosevka_16, 0);
    lv_obj_align(mac_lbl, LV_ALIGN_TOP_LEFT, 0, y_offset);

    lv_obj_t* mac_val = lv_label_create(parent);
    lv_label_set_text(mac_val, settings.beacon_count > 0 ? settings.beacon_macs[0] : "(not configured)");
    lv_obj_set_style_text_color(mac_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mac_val, &iosevka_16, 0);
    lv_obj_align(mac_val, LV_ALIGN_TOP_LEFT, 45, y_offset);
    y_offset += 40;

    // Found / Missing toggle button
    bool found = beacon_proximity::isFound();
    lv_obj_t* found_btn = lv_btn_create(parent);
    lv_obj_set_size(found_btn, 200, 48);
    lv_obj_align(found_btn, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_set_style_bg_color(found_btn,
        found ? lv_color_hex(0xBF5000) : lv_color_hex(0x1A3A1A), 0);
    lv_obj_set_style_bg_color(found_btn,
        found ? lv_color_hex(0x995500) : lv_color_hex(0x2A5A2A), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(found_btn,
        found ? lv_color_hex(0xFF8800) : lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_border_width(found_btn, 1, 0);

    lv_obj_t* found_lbl = lv_label_create(found_btn);
    lv_label_set_text(found_lbl, found ? "Found  - tap to reset" : "Missing - scanning");
    lv_obj_set_style_text_font(found_lbl, &iosevka_16, 0);
    lv_obj_set_style_text_color(found_lbl,
        found ? lv_color_hex(0xFFAA33) : lv_color_hex(0x00CC00), 0);
    lv_obj_center(found_lbl);

    lv_obj_add_event_cb(found_btn, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        bool now_found = beacon_proximity::isFound();
        beacon_proximity::setFound(!now_found);
        buzzer::chirp(30);
        // Update button appearance in-place
        lv_obj_t* btn = lv_event_get_target(e);
        bool new_found = beacon_proximity::isFound();
        lv_obj_set_style_bg_color(btn,
            new_found ? lv_color_hex(0xBF5000) : lv_color_hex(0x1A3A1A), 0);
        lv_obj_set_style_border_color(btn,
            new_found ? lv_color_hex(0xFF8800) : lv_color_hex(0x00AA00), 0);
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, new_found ? "Found  - tap to reset" : "Missing - scanning");
            lv_obj_set_style_text_color(lbl,
                new_found ? lv_color_hex(0xFFAA33) : lv_color_hex(0x00CC00), 0);
        }
    }, LV_EVENT_CLICKED, nullptr);
    y_offset += 60;

    // Hint about add/delete/pair
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, "Multi-beacon management coming soon.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(hint, &iosevka_16, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, y_offset);

    // Bottom spacer
    lv_obj_t* spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 1, 100);
    lv_obj_align(spacer, LV_ALIGN_TOP_LEFT, 0, y_offset + 30);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    Serial.println("[SETTINGS] Beacon tab created");
}

void updateWiFiStatus() {
    // WiFi STA is now a separate boot mode — no status label in settings screen.
}

void updateAPModeStatus() {
    // Only update if AP status label exists
    if (!g_wifi_ap_status_label) {
        return;
    }

    // Check if AP mode is active: server must be running AND AP mode explicitly enabled.
    // GPX server also starts when WiFi station connects — don't show AP status for that.
    bool ap_active = gpx_server::isRunning() && settings_manager::getSettings().wifi_ap_enabled;

    // Static state tracking to avoid excessive logging
    static bool last_ap_active = false;
    static uint8_t last_num_stations = 0;

    if (ap_active) {
        // Get AP IP via gpx_server
        char ip_str[16] = "192.168.4.1";
        gpx_server::getStatus(ip_str, sizeof(ip_str));

        // Get connected station count via ESP-IDF
        wifi_sta_list_t sta_list = {};
        // Guard: returns error if driver is in a transient state (mode switch, stop/start)
        if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) sta_list.num = 0;
        uint8_t num_stations = (uint8_t)sta_list.num;

        char status_text[128];
        const auto& ap_st = settings_manager::getSettings();
        snprintf(status_text, sizeof(status_text),
                 "AP: %s @ %s\nConnected devices: %d\nPassword: %s",
                 ap_st.ap_ssid, ip_str, num_stations, ap_st.ap_password);

        lv_label_set_text(g_wifi_ap_status_label, status_text);
        lv_obj_clear_flag(g_wifi_ap_status_label, LV_OBJ_FLAG_HIDDEN);

        if (!last_ap_active) {
            Serial.printf("[WIFI_AP_STATUS] AP active - IP: %s\n", ip_str);
        } else if (num_stations != last_num_stations) {
            Serial.printf("[WIFI_AP_STATUS] Connected devices: %d\n", num_stations);
        }

        last_ap_active = true;
        last_num_stations = num_stations;
    } else {
        // Hide status label when AP is not active
        lv_obj_add_flag(g_wifi_ap_status_label, LV_OBJ_FLAG_HIDDEN);

        // Only log when AP stops (state change)
        if (last_ap_active) {
            Serial.println("[WIFI_AP_STATUS] AP not active");
        }

        last_ap_active = false;
        last_num_stations = 0;
    }
}

} // namespace settings_screen

// ============================================================================
// GPS Help Modal Implementation
// ============================================================================

static void showHelpModal(const char* title, const char* content) {
    Serial.printf("[HELP] Showing help modal: %s\n", title);

    // Create modal background
    g_help_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_help_modal, 480, 480);
    lv_obj_set_style_bg_color(g_help_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_help_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(g_help_modal, 0, 0);
    lv_obj_clear_flag(g_help_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Create dialog box
    lv_obj_t* dialog_box = lv_obj_create(g_help_modal);
    lv_obj_set_size(dialog_box, 360, 400);
    lv_obj_align(dialog_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog_box, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(dialog_box, 2, 0);
    lv_obj_set_style_border_color(dialog_box, lv_color_hex(0x0080FF), 0);
    lv_obj_clear_flag(dialog_box, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* lbl_title = lv_label_create(dialog_box);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x00AAFF), 0);  // Blue title
    lv_obj_set_pos(lbl_title, 10, 10);

    // Scrollable content area
    lv_obj_t* content_container = lv_obj_create(dialog_box);
    lv_obj_set_size(content_container, 340, 290);
    lv_obj_set_pos(content_container, 10, 35);
    lv_obj_set_style_bg_color(content_container, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(content_container, 1, 0);
    lv_obj_set_style_border_color(content_container, lv_color_hex(0x404040), 0);
    lv_obj_set_scrollbar_mode(content_container, LV_SCROLLBAR_MODE_AUTO);

    // Content label (scrollable)
    lv_obj_t* lbl_content = lv_label_create(content_container);
    lv_label_set_text(lbl_content, content);
    lv_obj_set_width(lbl_content, 320);
    lv_obj_set_style_text_color(lbl_content, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(lbl_content, &iosevka_16, 0);
    lv_label_set_long_mode(lbl_content, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_content, 5, 5);

    // Close button at bottom
    lv_obj_t* btn_close = lv_btn_create(dialog_box);
    lv_obj_set_size(btn_close, 340, 45);
    lv_obj_set_pos(btn_close, 10, 340);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x666666), 0);
    lv_obj_add_event_cb(btn_close, onHelpModalClose, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_close);

    Serial.println("[HELP] Help modal created");
}

static void onHelpModalClose(lv_event_t* e) {
    Serial.println("[HELP] Help modal close button clicked");

    if (g_help_modal) {
        lv_obj_del(g_help_modal);
        g_help_modal = nullptr;
    }
}


// ============================================================================
// GPX Waypoint Management Implementation
// ============================================================================

static void updateWaypointCountLabel() {
    if (!g_waypoint_count_label) {
        return;
    }

    int current_count = gpx_loader::getWaypointCount();
    const int max_count = ui_manager::RadarConfig::MAX_WAYPOINTS;

    // Format: "📍 Waypoints: 15/50"
    char count_text[32];
    snprintf(count_text, sizeof(count_text), "Waypoints: %d/%d", current_count, max_count);
    lv_label_set_text(g_waypoint_count_label, count_text);

    // Color coding based on usage
    lv_color_t color;
    if (current_count <= 30) {
        color = lv_color_hex(0x00FF00);  // Green: 0-30 waypoints
    } else if (current_count <= 45) {
        color = lv_color_hex(0xFFFF00);  // Yellow: 31-45 waypoints
    } else {
        color = lv_color_hex(0xFF4444);  // Red: 46-50 waypoints (approaching limit)
    }
    lv_obj_set_style_text_color(g_waypoint_count_label, color, 0);

    Serial.printf("[GPX_SETTINGS] Waypoint count updated: %d/%d\n", current_count, max_count);
}

static void onRefreshWaypoints(lv_event_t* e) {
    Serial.println("[GPX_SETTINGS] Refresh Waypoints button clicked");

    // Reload all GPX files from SD card
    int waypoints_loaded = gpx_loader::refreshGPXFiles();

    if (waypoints_loaded > 0) {
        // Update radar display to show new waypoints
        navigation::updateRadarDisplay();

        // Update waypoint count label
        updateWaypointCountLabel();
    } else if (waypoints_loaded == 0) {
        Serial.println("[GPX] No waypoints found on SD card");
        updateWaypointCountLabel();  // Update to show 0
    } else {
        Serial.println("[GPX] Failed to refresh waypoints (SD card error?)");
    }
}

