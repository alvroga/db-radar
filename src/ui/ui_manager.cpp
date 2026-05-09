#include "ui_manager.h"
#include "settings_screen.h"
#include "system_config.h"
#include "navigation.h"
#include "gps_bh880.h"
#include "settings_manager.h"
#include "utils/system_logger.h"
#include "core/arduino_compat.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "hardware/connectivity/wifi_manager.h"
#include "hardware/connectivity/scanner.h"
#include "gpx/gpx_server.h"

// Shadow overlay image (C linkage)
extern "C" {
}
// Iosevka fonts declared via custom_fonts.h (included through ui_manager.h)

namespace ui_manager {

// Define constexpr static arrays (required for C++11)
constexpr ZoomConfig RadarConfig::ZOOM_CONFIGS[];

// Global state
static Config g_config;
static UIState g_ui_state;

UIState& getUIState() {
    return g_ui_state;
}

bool init(const Config& config) {
    g_config = config;

    // Set root screen background (15% grey)
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x262626), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Create circular stage (clips children)
    lv_obj_t* stage = lv_obj_create(scr);
    lv_obj_set_size(stage, g_config.screen_width, g_config.screen_height);
    lv_obj_align(stage, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(stage, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(stage, true, 0);
    lv_obj_set_style_border_width(stage, 2, 0);
    lv_obj_set_style_border_color(stage, lv_color_hex(0x303030), 0);

    // Header removed - radar is full screen now
    g_ui_state.header = nullptr;
    g_ui_state.header_label = nullptr;
    g_ui_state.btn_back = nullptr;

    // Load settings from cache (including heading_up_mode)
    const auto& settings = settings_manager::getSettings();
    g_ui_state.heading_up_mode = settings.heading_up_mode;  // Apply navigation mode
    Serial.printf("[UI] Navigation mode loaded: %s\n",
                  g_ui_state.heading_up_mode ? "Heading-Up" : "North-Up");

    // Create radar screen directly as the main screen
    createRadarScreen();

    // Save radar screen as main screen reference
    g_ui_state.screen_main = g_ui_state.screen_radar;
    lv_scr_load(g_ui_state.screen_radar);

    return true;
}

// createMainScreen removed - radar is now the default screen

void createLoadingScreen() {
    Serial.println("[LOADING] Creating loading screen with ease-in-out spinner...");

    // Create new screen for loading
    g_ui_state.screen_loading = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_ui_state.screen_loading, lv_color_hex(0x262626), 0);  // Dark grey background
    lv_obj_set_style_bg_opa(g_ui_state.screen_loading, LV_OPA_COVER, 0);

    // Create circular clipping stage
    lv_obj_t* stage = lv_obj_create(g_ui_state.screen_loading);
    lv_obj_set_size(stage, g_config.screen_width, g_config.screen_height);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_style_bg_color(stage, lv_color_hex(0x262626), 0);  // Dark grey
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stage, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(stage, true, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);

    // Title label
    lv_obj_t* title_label = lv_label_create(stage);
    lv_label_set_text(title_label, "DRAC OS");
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, &iosevka_20, 0);  // Custom Iosevka 20pt font
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, -60);  // Closer to spinner (was -100)

    // Version label — baked in at build time via scripts/gen_version.py
    lv_obj_t* version_label = lv_label_create(stage);
    lv_label_set_text(version_label, FW_VERSION);
    lv_obj_set_style_text_color(version_label, lv_color_hex(0x00FF00), 0);  // Radar green
    lv_obj_set_style_text_font(version_label, &iosevka_14, 0);
    lv_obj_align(version_label, LV_ALIGN_BOTTOM_MID, 0, -50);  // Bottom of circle, 50px inset

    // Create spinner with ease-in-out animation
    // LVGL 8.x: lv_spinner_create(parent, spin_time_ms, arc_length_deg)
    lv_obj_t* spinner = lv_spinner_create(stage, 2000, 60);  // 2 second rotation, 60° arc
    lv_obj_set_size(spinner, 75, 75);  // 75px diameter (50% smaller)
    lv_obj_center(spinner);

    // Customize spinner appearance
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);  // Proportionally thinner for smaller size
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x3A9949), LV_PART_MAIN);  // Green (radar color)
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x00FF00), LV_PART_INDICATOR);  // Bright green

    // Apply ease-in-out animation curve to spinner
    // The spinner internally uses an arc widget with a rotation animation
    // We need to modify the animation's path callback to use ease_in_out

    // LVGL 8.x: Access the spinner's internal animation
    // Spinners use lv_anim internally - we'll search for the arc's angle animation
    lv_anim_t* anim = lv_anim_get(spinner, nullptr);  // Get first animation on spinner
    if (anim) {
        // Set ease-in-out path for smooth acceleration/deceleration
        lv_anim_set_path_cb(anim, lv_anim_path_ease_in_out);
        Serial.println("[LOADING] ✓ Ease-in-out animation applied to spinner");
    } else {
        // Fallback: Animation might not be accessible yet, but default linear is acceptable
        Serial.println("[LOADING] Note: Using default spinner animation (ease-in-out may not be applied)");
    }

    // Status label - "Initializing..." (stored for dynamic updates)
    g_ui_state.loading_status_label = lv_label_create(stage);
    lv_label_set_text(g_ui_state.loading_status_label, "Initializing...");
    lv_obj_set_style_text_color(g_ui_state.loading_status_label, lv_color_hex(0xAAAAAA), 0);  // Light grey
    lv_obj_set_style_text_font(g_ui_state.loading_status_label, &iosevka_16, 0);  // Custom Iosevka 16pt font
    lv_obj_align(g_ui_state.loading_status_label, LV_ALIGN_CENTER, 0, 60);  // Closer to spinner (was 100)

    Serial.println("[LOADING] Loading screen created successfully");
}

void updateLoadingStatus(const char* message) {
    if (g_ui_state.loading_status_label && lv_obj_is_valid(g_ui_state.loading_status_label)) {
        lv_label_set_text(g_ui_state.loading_status_label, message);
        // NOTE: Do NOT call lv_timer_handler() here.
        // After FreeRTOS tasks start, the UI Task owns all LVGL processing.
        // Calling lv_timer_handler() from setup()/loop() while the UI Task runs
        // on the same core causes LVGL re-entrancy corruption and UI Task hangs.
        // The UI Task processes LVGL every 10ms, so updates appear quickly.
    }
}

void createRadarScreen() {
    // Create new screen for radar
    g_ui_state.screen_radar = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_ui_state.screen_radar, lv_color_hex(0x3A9949), 0);  // Green background
    lv_obj_set_style_bg_opa(g_ui_state.screen_radar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_ui_state.screen_radar, 0, 0);  // Remove all padding from screen

    // Create circular clipping stage
    lv_obj_t* stage = lv_obj_create(g_ui_state.screen_radar);
    lv_obj_set_size(stage, g_config.screen_width, g_config.screen_height);
    lv_obj_set_pos(stage, 0, 0);  // Explicitly set position to (0,0)
    lv_obj_set_style_bg_color(stage, lv_color_hex(0x3A9949), 0);  // Green background
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stage, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(stage, true, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);  // Remove all padding
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    // Add touch event handler: HUD control + waypoint tap detection
    lv_obj_add_event_cb(stage, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
            onScreenTouch();
            // Check if the tap landed on a waypoint dot
            lv_indev_t* indev = lv_indev_get_act();
            if (indev) {
                lv_point_t pt;
                lv_indev_get_point(indev, &pt);
                navigation::handleTapAt(pt.x, pt.y);
            }
        }
    }, LV_EVENT_PRESSED, nullptr);

    // Allocate canvas draw buffer (RGB565 format - LVGL 8.x)
    size_t canvas_buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(g_config.screen_width, g_config.screen_height);
    Serial.printf("[RADAR] Canvas buffer size needed: %u bytes (%.1f KB)\n", canvas_buf_size, canvas_buf_size / 1024.0);

    // Try SPIRAM first (8MB available), then fallback to DMA-capable RAM
    void* canvas_buf = heap_caps_malloc(canvas_buf_size, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        Serial.println("[RADAR] WARNING: SPIRAM allocation failed, trying DMA RAM...");
        canvas_buf = heap_caps_malloc(canvas_buf_size, MALLOC_CAP_DMA);
    }

    if (!canvas_buf) {
        Serial.println("[RADAR] ERROR: Failed to allocate canvas buffer!");
        Serial.printf("[RADAR] Free heap: %u bytes\n", (unsigned)esp_get_free_heap_size());
        Serial.printf("[RADAR] Free PSRAM: %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        Serial.printf("[RADAR] Largest free block: %u bytes\n", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        return;
    }

    Serial.printf("[RADAR] Canvas buffer allocated: %p (%u bytes)\n", canvas_buf, canvas_buf_size);

    // Create canvas (LVGL 8.x API) - full screen, no header
    g_ui_state.radar_canvas = lv_canvas_create(stage);
    lv_canvas_set_buffer(g_ui_state.radar_canvas, canvas_buf, g_config.screen_width, g_config.screen_height, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(g_ui_state.radar_canvas, g_config.screen_width, g_config.screen_height);
    lv_obj_set_pos(g_ui_state.radar_canvas, 0, 0);  // Explicitly set position to (0,0)
    lv_obj_set_style_pad_all(g_ui_state.radar_canvas, 0, 0);  // Remove all padding

    Serial.printf("[RADAR] Canvas size: %dx%d, positioned at (0,0)\n",
                  g_config.screen_width, g_config.screen_height);

    // Fill canvas with green background
    lv_canvas_fill_bg(g_ui_state.radar_canvas, lv_color_hex(0x3A9949), LV_OPA_COVER);

    // Touch-to-zoom disabled - touch will be used for other functionalities
    // Future: Touch can be used for waypoint selection, dragging, etc.
    // lv_obj_add_flag(g_ui_state.radar_canvas, LV_OBJ_FLAG_CLICKABLE);
    // lv_obj_add_event_cb(g_ui_state.radar_canvas, [](lv_event_t* e) { ... }, LV_EVENT_CLICKED, nullptr);

    // Add zoom level indicator (always visible, above GPS status)
    g_ui_state.zoom_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.zoom_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.zoom_label, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.zoom_label, "[100m]");  // Default zoom
    lv_obj_align(g_ui_state.zoom_label, LV_ALIGN_RIGHT_MID, -15, 40);
    Serial.println("[RADAR] Zoom level label created (always visible)");

    // Add GPS status indicator (bottom-mid) - shows satellite count
    g_ui_state.gps_status_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.gps_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.gps_status_label, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.gps_status_label, "GPS: Searching...");
    lv_obj_align(g_ui_state.gps_status_label, LV_ALIGN_BOTTOM_MID, 0, -55);
    Serial.println("[RADAR] GPS status label created");

    // Add GPS quality indicator (below GPS status) - shows HDOP and speed
    g_ui_state.gps_quality_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.gps_quality_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.gps_quality_label, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.gps_quality_label, "");  // Hidden until GPS fix
    lv_obj_align(g_ui_state.gps_quality_label, LV_ALIGN_BOTTOM_MID, 0, -35);
    Serial.println("[RADAR] GPS quality label created");

    // Add battery status indicator (top-right, safe margin)
    g_ui_state.battery_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.battery_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.battery_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(g_ui_state.battery_label, "--%");  // Short format: just percentage
    lv_obj_align(g_ui_state.battery_label, LV_ALIGN_TOP_RIGHT, -150, 20);
    Serial.println("[RADAR] Battery label created");

    // Add log indicator (top-left, safe margin from circular edge)
    g_ui_state.log_indicator = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.log_indicator, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.log_indicator, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.log_indicator, "DEV");
    lv_obj_align(g_ui_state.log_indicator, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_ui_state.log_indicator, LV_OBJ_FLAG_HIDDEN);
    Serial.println("[RADAR] Log indicator created (DEV mode)");

    // Beacon dBm label (DEV mode, 50m zoom only — for field calibration)
    g_ui_state.beacon_dbm_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.beacon_dbm_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.beacon_dbm_label, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.beacon_dbm_label, "");
    lv_obj_align(g_ui_state.beacon_dbm_label, LV_ALIGN_BOTTOM_LEFT, 80, -80);
    lv_obj_add_flag(g_ui_state.beacon_dbm_label, LV_OBJ_FLAG_HIDDEN);
    Serial.println("[RADAR] Beacon dBm label created (DEV mode)");


    // Fixed waypoint distance label — bottom-center, hidden until a waypoint is fixed.
    // Tappable: opens the waypoint detail screen so the user can read info or unfix.
    g_ui_state.waypoint_distance_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.waypoint_distance_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_ui_state.waypoint_distance_label, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.waypoint_distance_label, "Fixed: ---");
    lv_obj_align(g_ui_state.waypoint_distance_label, LV_ALIGN_LEFT_MID, 15, -20);
    lv_obj_add_flag(g_ui_state.waypoint_distance_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui_state.waypoint_distance_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(g_ui_state.waypoint_distance_label, 20);
    lv_obj_add_event_cb(g_ui_state.waypoint_distance_label, [](lv_event_t* e) {
        ui_manager::UIState& ui = ui_manager::getUIState();
        if (ui.fixed_waypoint_index >= 0) {
            ui.selected_waypoint_index = ui.fixed_waypoint_index;
            navigation::goToWaypointScreen();
        }
    }, LV_EVENT_CLICKED, nullptr);

    // Beacon-found indicator — 40×40 TRUE_COLOR_ALPHA canvas, same position as DEV label.
    // Shows a circle outline + solid star when beacon_found is set in NVS.
    // Hides with the rest of the HUD. Managed by updateRadarDisplay().
    {
        static uint8_t s_beacon_found_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(40, 40)];
        g_ui_state.beacon_found_canvas = lv_canvas_create(stage);
        lv_canvas_set_buffer(g_ui_state.beacon_found_canvas,
                             s_beacon_found_buf, 40, 40, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_obj_set_size(g_ui_state.beacon_found_canvas, 40, 40);
        lv_obj_align(g_ui_state.beacon_found_canvas, LV_ALIGN_TOP_LEFT, 80, 60);
        lv_obj_add_flag(g_ui_state.beacon_found_canvas, LV_OBJ_FLAG_HIDDEN);
        navigation::drawBeaconFoundIndicator(g_ui_state.beacon_found_canvas, false);
    }

    // WiFi mode overlay — hidden by default, shown when WiFi disables the radar
    g_ui_state.wifi_mode_label = lv_label_create(stage);
    lv_obj_set_style_text_color(g_ui_state.wifi_mode_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_ui_state.wifi_mode_label, &iosevka_16, 0);
    lv_label_set_text(g_ui_state.wifi_mode_label, "WIFI MODE\nRadar disabled\nGPX upload active");
    lv_obj_set_style_text_align(g_ui_state.wifi_mode_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_ui_state.wifi_mode_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_ui_state.wifi_mode_label, LV_OBJ_FLAG_HIDDEN);  // Hidden unless WiFi active

    // Initialize waypoint system
    g_ui_state.waypoint_count = 0;

    // Center reference will be updated to GPS position automatically
    // Initial default position (will be replaced when GPS gets fix)
    g_ui_state.center_lat = 34.133417;
    g_ui_state.center_lon = -118.145190;

    Serial.println("[RADAR] Radar screen created");

    // Force initial radar display update to draw grid, beacons, and center triangle
    navigation::updateRadarDisplay();
    Serial.println("[RADAR] Initial display update complete");

    // Initialize HUD visibility state
    g_ui_state.hud_visible = true;
    g_ui_state.last_interaction_ms = millis();  // Start timer for auto-hide

    // Load HUD settings from NVS (cached to avoid repeated reads)
    reloadHUDSettings();

    // Show DEV label on boot only when dev_mode is enabled
    if (g_ui_state.log_indicator && settings_manager::getSettings().dev_mode) {
        lv_obj_clear_flag(g_ui_state.log_indicator, LV_OBJ_FLAG_HIDDEN);
        Serial.println("[RADAR] DEV label visible (dev_mode ON)");
    }
}

// Settings screen moved to settings_screen.cpp
void createSettingsScreen() {
    settings_screen::create();
}

// HUD visibility management

void updateHUDVisibility() {
    // Use cached settings to avoid excessive NVS reads
    // Only auto-hide if enabled and duration is not 0 (Never)
    if (!g_ui_state.hud_auto_hide_enabled || g_ui_state.hud_auto_hide_delay_ms == 0) {
        return;  // Auto-hide disabled
    }

    // Auto-hide HUD after configured delay
    if (g_ui_state.hud_visible && g_ui_state.last_interaction_ms > 0) {
        uint32_t elapsed = millis() - g_ui_state.last_interaction_ms;
        if (elapsed >= g_ui_state.hud_auto_hide_delay_ms) {
            hideHUD();
        }
    }
}

void reloadHUDSettings() {
    // Load HUD settings from cache
    const auto& settings = settings_manager::getSettings();

    g_ui_state.hud_auto_hide_enabled = settings.hud_auto_hide;
    g_ui_state.hud_auto_hide_delay_ms = settings.hud_auto_hide_seconds * 1000;

    Serial.printf("[UI] HUD settings reloaded: auto_hide=%s, delay=%lums\n",
                  g_ui_state.hud_auto_hide_enabled ? "ON" : "OFF",
                  g_ui_state.hud_auto_hide_delay_ms);
}

void showHUD() {
    if (!g_ui_state.hud_visible) {
        if (g_ui_state.gps_status_label)
            lv_obj_clear_flag(g_ui_state.gps_status_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.gps_quality_label)
            lv_obj_clear_flag(g_ui_state.gps_quality_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.battery_label)
            lv_obj_clear_flag(g_ui_state.battery_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.zoom_label)
            lv_obj_clear_flag(g_ui_state.zoom_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.log_indicator && settings_manager::getSettings().dev_mode)
            lv_obj_clear_flag(g_ui_state.log_indicator, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.beacon_dbm_label)
            lv_obj_clear_flag(g_ui_state.beacon_dbm_label, LV_OBJ_FLAG_HIDDEN);
        g_ui_state.hud_visible = true;
        Serial.println("[UI] HUD shown");
    }
}

void hideHUD() {
    if (g_ui_state.hud_visible) {
        if (g_ui_state.gps_status_label)
            lv_obj_add_flag(g_ui_state.gps_status_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.gps_quality_label)
            lv_obj_add_flag(g_ui_state.gps_quality_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.battery_label)
            lv_obj_add_flag(g_ui_state.battery_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.zoom_label)
            lv_obj_add_flag(g_ui_state.zoom_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.log_indicator)
            lv_obj_add_flag(g_ui_state.log_indicator, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.beacon_dbm_label)
            lv_obj_add_flag(g_ui_state.beacon_dbm_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.waypoint_distance_label)
            lv_obj_add_flag(g_ui_state.waypoint_distance_label, LV_OBJ_FLAG_HIDDEN);
        if (g_ui_state.beacon_found_canvas)
            lv_obj_add_flag(g_ui_state.beacon_found_canvas, LV_OBJ_FLAG_HIDDEN);
        g_ui_state.hud_visible = false;
        Serial.println("[UI] HUD hidden");
    }
}

void onScreenTouch() {
    // Record interaction time
    g_ui_state.last_interaction_ms = millis();

    // Show HUD if hidden
    if (!g_ui_state.hud_visible) {
        showHUD();
    }
}

// Utility functions

void showBackButton(bool show) {
    if (!g_ui_state.btn_back) return;
    if (show) lv_obj_clear_flag(g_ui_state.btn_back, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(g_ui_state.btn_back, LV_OBJ_FLAG_HIDDEN);
}

void setHeaderTitle(const char* title) {
    if (g_ui_state.header_label) {
        lv_label_set_text(g_ui_state.header_label, title);
    }
}


// Test callback implementations removed

// Memory management implementations - simplified for radar project
void cleanupScreen(lv_obj_t* screen) {
    if (!screen) return;
    Serial.printf("[UI] Cleaning up screen: %p\n", (void*)screen);
    lv_obj_del(screen);
}

void cleanupAllScreens() {
    Serial.println("[UI] Cleanup not needed - radar uses single main screen");
}

bool safeScreenLoad(lv_obj_t* screen) {
    if (!screen) {
        Serial.println("[UI] ERROR: Attempting to load null screen");
        return false;
    }

    // Check if screen is valid by testing if it's part of LVGL's object tree
    if (!lv_obj_is_valid(screen)) {
        Serial.println("[UI] ERROR: Attempting to load invalid screen object");
        return false;
    }

    // Load the screen safely
    lv_scr_load(screen);
    return true;
}

void stopAllTimers() {
    Serial.println("[UI] No timers to stop in radar project");
}

void updateDaylightMode(bool daylight_enabled) {
    // Update HUD text colors for daylight mode (black text for visibility on light background)
    // Note: Battery label color is managed by task_manager based on battery level
    // We only change the base color here; battery updates will override with appropriate color

    // Update zoom label text color
    if (g_ui_state.zoom_label) {
        lv_obj_set_style_text_color(g_ui_state.zoom_label,
            daylight_enabled ? lv_color_black() : lv_color_white(), 0);
    }

    // Update GPS status text color
    if (g_ui_state.gps_status_label) {
        lv_obj_set_style_text_color(g_ui_state.gps_status_label,
            daylight_enabled ? lv_color_black() : lv_color_white(), 0);
    }

    // Update battery text color
    if (g_ui_state.battery_label) {
        lv_obj_set_style_text_color(g_ui_state.battery_label,
            daylight_enabled ? lv_color_black() : lv_color_white(), 0);
    }

    // Update log indicator (DEV) text color
    if (g_ui_state.log_indicator) {
        lv_obj_set_style_text_color(g_ui_state.log_indicator,
            daylight_enabled ? lv_color_black() : lv_color_white(), 0);
    }

    // Update beacon dBm label text color
    if (g_ui_state.beacon_dbm_label) {
        lv_obj_set_style_text_color(g_ui_state.beacon_dbm_label,
            daylight_enabled ? lv_color_black() : lv_color_white(), 0);
    }

    // Update waypoint distance label text color
    if (g_ui_state.waypoint_distance_label) {
        lv_obj_set_style_text_color(g_ui_state.waypoint_distance_label,
            daylight_enabled ? lv_color_black() : lv_color_white(), 0);
    }

    // Redraw beacon-found indicator in new theme color
    if (g_ui_state.beacon_found_canvas) {
        navigation::drawBeaconFoundIndicator(g_ui_state.beacon_found_canvas, daylight_enabled);
    }

    Serial.printf("[UI] Daylight mode %s - HUD text: %s\n",
                  daylight_enabled ? "ENABLED" : "DISABLED",
                  daylight_enabled ? "black" : "white");
}

// ============================================================================
// AP Upload Mode Screen
// ============================================================================

// Module-static label pointer used by the 2-second update timer
static lv_obj_t* s_ap_connected_label = nullptr;

void createAPScreen() {
    UIState& ui = getUIState();

    ui.screen_ap = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(ui.screen_ap, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(ui.screen_ap, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui.screen_ap, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title ──────────────────────────────────────────────────────────────
    lv_obj_t* title = lv_label_create(ui.screen_ap);
    lv_label_set_text(title, "GPS Radar - Upload Mode");
    lv_obj_set_style_text_font(title, &iosevka_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFAA), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // ── Network info card ─────────────────────────────────────────────────
    lv_obj_t* card = lv_obj_create(ui.screen_ap);
    lv_obj_set_size(card, 400, 220);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111122), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00FFAA), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // SSID row
    lv_obj_t* lbl_ssid = lv_label_create(card);
    lv_label_set_text_fmt(lbl_ssid, "Network:   %s", settings_manager::getSettings().ap_ssid);
    lv_obj_set_style_text_font(lbl_ssid, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_ssid, lv_color_white(), 0);
    lv_obj_align(lbl_ssid, LV_ALIGN_TOP_LEFT, 15, 15);

    // Password row
    lv_obj_t* lbl_pass = lv_label_create(card);
    lv_label_set_text_fmt(lbl_pass, "Password:  %s", settings_manager::getSettings().ap_password);
    lv_obj_set_style_text_font(lbl_pass, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_pass, lv_color_white(), 0);
    lv_obj_align(lbl_pass, LV_ALIGN_TOP_LEFT, 15, 50);

    // URL row
    lv_obj_t* lbl_url = lv_label_create(card);
    lv_label_set_text(lbl_url, "Address:   http://192.168.4.1");
    lv_obj_set_style_text_font(lbl_url, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_url, lv_color_hex(0x66CCFF), 0);
    lv_obj_align(lbl_url, LV_ALIGN_TOP_LEFT, 15, 85);

    // Divider
    lv_obj_t* divider = lv_obj_create(card);
    lv_obj_set_size(divider, 370, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 15, 125);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x333355), 0);
    lv_obj_set_style_border_width(divider, 0, 0);

    // Connected devices (updated by timer)
    s_ap_connected_label = lv_label_create(card);
    lv_label_set_text(s_ap_connected_label, "Connected: 0 devices");
    lv_obj_set_style_text_font(s_ap_connected_label, &iosevka_16, 0);
    lv_obj_set_style_text_color(s_ap_connected_label, lv_color_hex(0xAAFFAA), 0);
    lv_obj_align(s_ap_connected_label, LV_ALIGN_TOP_LEFT, 15, 140);

    // Status note
    lv_obj_t* lbl_note = lv_label_create(card);
    lv_label_set_text(lbl_note, "Open browser and navigate to the address above");
    lv_obj_set_style_text_font(lbl_note, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_note, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(lbl_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_note, 370);
    lv_obj_align(lbl_note, LV_ALIGN_TOP_LEFT, 15, 175);

    // ── Reboot button ─────────────────────────────────────────────────────
    lv_obj_t* btn = lv_btn_create(ui.screen_ap);
    lv_obj_set_size(btn, 280, 52);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF6600), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF8800), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, [](lv_event_t*) {
        Serial.println("[AP_SCREEN] Reboot to Radar tapped — clearing AP mode flag");
        settings_manager::saveWiFiAPEnabled(false);
        esp_restart();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Reboot to Radar");
    lv_obj_set_style_text_font(btn_lbl, &iosevka_20, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_center(btn_lbl);

    // ── 2-second timer to update connected device count ───────────────────
    lv_timer_create([](lv_timer_t*) {
        if (!s_ap_connected_label) return;
        wifi_sta_list_t list = {};
        if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return;
        char buf[40];
        snprintf(buf, sizeof(buf), "Connected: %d device%s",
                 list.num, list.num == 1 ? "" : "s");
        lv_label_set_text(s_ap_connected_label, buf);
    }, 2000, nullptr);

    Serial.println("[UI] AP upload screen created");
}

// ============================================================================
// WiFi STA Mode Screen
// ============================================================================

static lv_obj_t*   s_wifi_network_label = nullptr;
static lv_obj_t*   s_wifi_address_label = nullptr;
static lv_obj_t*   s_wifi_note_label    = nullptr;
static lv_obj_t*   s_wifi_forget_btn    = nullptr;
static lv_obj_t*   s_wifi_network_modal = nullptr;
static lv_timer_t* s_wifi_scan_timer    = nullptr;

static struct {
    lv_obj_t* dialog   = nullptr;
    lv_obj_t* textarea = nullptr;
    lv_obj_t* keyboard = nullptr;
    String    ssid;
    wifi_auth_mode_t auth_type = WIFI_AUTH_OPEN;
} s_wifi_pwd;

// Forward declarations
static void wifiOnNetworkSelected(lv_event_t* e);
static void wifiOnPasswordConnect(lv_event_t* e);
static void wifiOnPasswordCancel(lv_event_t* e);
static void wifiOnNetworkListClose(lv_event_t* e);
static void wifiShowPasswordDialog(const String& ssid, wifi_auth_mode_t auth_type);

// ---------------------------------------------------------------------------
// Populate the scan results list — direct port from original settings_screen.cpp.
// ---------------------------------------------------------------------------
static void wifiPopulateList(lv_obj_t* network_list) {
    lv_obj_clean(network_list);

    int count = scanner::scanComplete();

    if (count == scanner::SCAN_RUNNING) {
        lv_obj_t* item = lv_list_add_text(network_list, "Scanning...");
        lv_obj_set_style_text_color(item, lv_color_hex(0x888888), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        return;
    }
    if (count == scanner::SCAN_FAILED) {
        lv_obj_t* item = lv_list_add_text(network_list, "Scan failed");
        lv_obj_set_style_text_color(item, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        return;
    }
    if (count == 0) {
        lv_obj_t* item = lv_list_add_text(network_list, "No networks found");
        lv_obj_set_style_text_color(item, lv_color_hex(0x888888), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        return;
    }

    Serial.printf("[WIFI] Populating list with %d networks\n", count);

    for (int i = 0; i < count; i++) {
        const scanner::APRecord* rec = scanner::getScanRecord(i);
        if (!rec) continue;
        String ssid = String(rec->ssid);
        if (ssid.isEmpty()) continue;

        int32_t rssi = rec->rssi;
        wifi_auth_mode_t encryption = rec->authmode;

        int strength;
        if      (rssi >= -50) strength = 4;
        else if (rssi >= -60) strength = 3;
        else if (rssi >= -70) strength = 2;
        else if (rssi >= -80) strength = 1;
        else                  strength = 0;

        char label_text[80];
        const char* lock_icon = (encryption == WIFI_AUTH_OPEN) ? "" : LV_SYMBOL_SETTINGS " ";
        snprintf(label_text, sizeof(label_text), "%s%s  %s (%ddBm)",
                 lock_icon, ssid.c_str(), LV_SYMBOL_WIFI, rssi);

        lv_obj_t* btn = lv_list_add_btn(network_list, nullptr, label_text);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

        lv_color_t color;
        switch (strength) {
            case 4: case 3: color = lv_color_hex(0x00FF00); break;
            case 2:         color = lv_color_hex(0xFFFF00); break;
            default:        color = lv_color_hex(0xFF8800); break;
        }
        lv_obj_set_style_text_color(btn, color, 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, wifiOnNetworkSelected, LV_EVENT_CLICKED, nullptr);
    }
    Serial.printf("[WIFI] List populated with %d networks\n", count);
}

// Drive the scan state machine — Network Task is not running in WiFi boot mode.
static void wifiScanCheckTimer(lv_timer_t* timer) {
    lv_obj_t* network_list = (lv_obj_t*)timer->user_data;
    scanner::update();  // collect results from WiFi driver (no Network Task in minimal boot)
    int scan_status = scanner::scanComplete();
    if (scan_status == scanner::SCAN_RUNNING) return;
    if (scan_status == scanner::SCAN_FAILED) {
        Serial.println("[WIFI] Scan failed");
        lv_obj_clean(network_list);
        lv_obj_t* item = lv_list_add_text(network_list, "Scan failed - try again");
        lv_obj_set_style_text_color(item, lv_color_hex(0xFF0000), 0);
        lv_timer_del(timer);
        s_wifi_scan_timer = nullptr;
        return;
    }
    Serial.printf("[WIFI] Scan complete, found %d networks\n", scan_status);
    wifiPopulateList(network_list);
    lv_timer_del(timer);
    s_wifi_scan_timer = nullptr;
}

static void wifiShowNetworkModal() {
    Serial.println("[WIFI] Showing network list modal");
    scanner::update();           // flush stale boot-time scan so triggerWiFiScan starts fresh
    scanner::triggerWiFiScan();

    s_wifi_network_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_wifi_network_modal, 480, 480);
    lv_obj_set_style_bg_color(s_wifi_network_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wifi_network_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_wifi_network_modal, 0, 0);
    lv_obj_clear_flag(s_wifi_network_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* dialog_box = lv_obj_create(s_wifi_network_modal);
    lv_obj_set_size(dialog_box, 360, 410);
    lv_obj_align(dialog_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog_box, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(dialog_box, 2, 0);
    lv_obj_set_style_border_color(dialog_box, lv_color_hex(0x0080FF), 0);
    lv_obj_clear_flag(dialog_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_title = lv_label_create(dialog_box);
    lv_label_set_text(lbl_title, "Select Network");
    lv_obj_set_style_text_font(lbl_title, &iosevka_16, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_title, 10, 10);

    lv_obj_t* list_container = lv_obj_create(dialog_box);
    lv_obj_set_size(list_container, 330, 320);
    lv_obj_set_pos(list_container, 0, 30);
    lv_obj_set_style_bg_color(list_container, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(list_container, 1, 0);
    lv_obj_set_style_border_color(list_container, lv_color_hex(0x404040), 0);
    lv_obj_set_scrollbar_mode(list_container, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* network_list = lv_list_create(list_container);
    lv_obj_set_size(network_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(network_list, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(network_list, 0, 0);
    lv_obj_set_style_bg_color(network_list, lv_color_hex(0x1A1A1A), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(network_list, LV_OPA_COVER, LV_PART_ITEMS);

    lv_obj_t* item = lv_list_add_text(network_list, "Scanning for networks...");
    lv_obj_set_style_text_color(item, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);

    lv_obj_t* btn_close = lv_btn_create(dialog_box);
    lv_obj_set_size(btn_close, 340, 45);
    lv_obj_set_pos(btn_close, 0, 350);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x666666), 0);
    lv_obj_add_event_cb(btn_close, wifiOnNetworkListClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_close);

    if (s_wifi_scan_timer) { lv_timer_del(s_wifi_scan_timer); }
    s_wifi_scan_timer = lv_timer_create(wifiScanCheckTimer, 500, network_list);
    Serial.println("[WIFI] Network list modal created, polling for scan results");
}

static void wifiShowPasswordDialog(const String& ssid, wifi_auth_mode_t auth_type) {
    Serial.printf("[WIFI] Showing password dialog for: %s\n", ssid.c_str());
    s_wifi_pwd.ssid      = ssid;
    s_wifi_pwd.auth_type = auth_type;

    s_wifi_pwd.dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_wifi_pwd.dialog, 480, 480);
    lv_obj_set_style_bg_color(s_wifi_pwd.dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wifi_pwd.dialog, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_wifi_pwd.dialog, 0, 0);
    lv_obj_clear_flag(s_wifi_pwd.dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* dialog_box = lv_obj_create(s_wifi_pwd.dialog);
    lv_obj_set_size(dialog_box, 360, 360);
    lv_obj_align(dialog_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog_box, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(dialog_box, 2, 0);
    lv_obj_set_style_border_color(dialog_box, lv_color_hex(0x0080FF), 0);
    lv_obj_clear_flag(dialog_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_ssid = lv_label_create(dialog_box);
    char ssid_buf[20];
    if (ssid.length() > 18) snprintf(ssid_buf, sizeof(ssid_buf), "%.15s...", ssid.c_str());
    else                     snprintf(ssid_buf, sizeof(ssid_buf), "%s", ssid.c_str());
    lv_label_set_text_fmt(lbl_ssid, "Network: %s", ssid_buf);
    lv_obj_set_style_text_color(lbl_ssid, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(lbl_ssid, &iosevka_16, 0);
    lv_obj_set_pos(lbl_ssid, 5, 8);

    lv_obj_t* cb_show = lv_checkbox_create(dialog_box);
    lv_checkbox_set_text(cb_show, "Show");
    lv_obj_set_pos(cb_show, 260, 5);
    lv_obj_set_style_text_color(cb_show, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(cb_show, &iosevka_16, 0);
    lv_obj_add_event_cb(cb_show, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            bool checked = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
            lv_textarea_set_password_mode(s_wifi_pwd.textarea, !checked);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    s_wifi_pwd.textarea = lv_textarea_create(dialog_box);
    lv_obj_set_size(s_wifi_pwd.textarea, 325, 40);
    lv_obj_set_pos(s_wifi_pwd.textarea, 5, 35);
    lv_textarea_set_placeholder_text(s_wifi_pwd.textarea, "Enter Password");
    lv_textarea_set_password_mode(s_wifi_pwd.textarea, true);
    lv_textarea_set_one_line(s_wifi_pwd.textarea, true);
    lv_obj_set_style_bg_color(s_wifi_pwd.textarea, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_text_color(s_wifi_pwd.textarea, lv_color_hex(0xFFFFFF), 0);

    s_wifi_pwd.keyboard = lv_keyboard_create(dialog_box);
    lv_obj_set_size(s_wifi_pwd.keyboard, 340, 200);
    lv_obj_set_pos(s_wifi_pwd.keyboard, 0, -40);
    lv_keyboard_set_textarea(s_wifi_pwd.keyboard, s_wifi_pwd.textarea);
    lv_obj_set_style_bg_color(s_wifi_pwd.keyboard, lv_color_hex(0x1A1A1A), 0);

    lv_obj_t* btn_connect = lv_btn_create(dialog_box);
    lv_obj_set_size(btn_connect, 160, 40);
    lv_obj_set_pos(btn_connect, 0, 295);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0x00AA00), 0);
    lv_obj_add_event_cb(btn_connect, wifiOnPasswordConnect, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_conn = lv_label_create(btn_connect);
    lv_label_set_text(lbl_conn, "Connect");
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_conn);

    lv_obj_t* btn_cancel = lv_btn_create(dialog_box);
    lv_obj_set_size(btn_cancel, 160, 40);
    lv_obj_set_pos(btn_cancel, 170, 295);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0xAA0000), 0);
    lv_obj_add_event_cb(btn_cancel, wifiOnPasswordCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_cancel);

    Serial.println("[WIFI] Password dialog created");
}

static void wifiOnNetworkSelected(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const scanner::APRecord* rec = scanner::getScanRecord(idx);
    if (!rec) return;
    String ssid = String(rec->ssid);
    if (ssid.isEmpty()) return;
    Serial.printf("[WIFI] Network selected: %s (%d dBm)\n", ssid.c_str(), rec->rssi);

    if (s_wifi_network_modal) { lv_obj_del(s_wifi_network_modal); s_wifi_network_modal = nullptr; }

    if (rec->authmode == WIFI_AUTH_OPEN) {
        wifi_manager::connect(ssid, "");
    } else {
        wifiShowPasswordDialog(ssid, rec->authmode);
    }
}

static void wifiOnPasswordConnect(lv_event_t* e) {
    const char* pw = lv_textarea_get_text(s_wifi_pwd.textarea);
    Serial.printf("[WIFI] Connecting to: %s\n", s_wifi_pwd.ssid.c_str());
    wifi_manager::connect(s_wifi_pwd.ssid, pw);
    if (s_wifi_pwd.dialog) {
        lv_obj_del(s_wifi_pwd.dialog);
        s_wifi_pwd.dialog = nullptr; s_wifi_pwd.textarea = nullptr; s_wifi_pwd.keyboard = nullptr;
    }
}

static void wifiOnPasswordCancel(lv_event_t* e) {
    if (s_wifi_pwd.dialog) {
        lv_obj_del(s_wifi_pwd.dialog);
        s_wifi_pwd.dialog = nullptr; s_wifi_pwd.textarea = nullptr; s_wifi_pwd.keyboard = nullptr;
    }
}

static void wifiOnNetworkListClose(lv_event_t* e) {
    if (s_wifi_scan_timer)    { lv_timer_del(s_wifi_scan_timer); s_wifi_scan_timer = nullptr; }
    if (s_wifi_network_modal) { lv_obj_del(s_wifi_network_modal); s_wifi_network_modal = nullptr; }
}

void createWiFiScreen() {
    UIState& ui = getUIState();

    ui.screen_wifi = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(ui.screen_wifi, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(ui.screen_wifi, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui.screen_wifi, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title ─────────────────────────────────────────────────────────────
    lv_obj_t* title = lv_label_create(ui.screen_wifi);
    lv_label_set_text(title, "GPS Radar - WiFi Mode");
    lv_obj_set_style_text_font(title, &iosevka_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFAA), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // ── Network info card ─────────────────────────────────────────────────
    lv_obj_t* card = lv_obj_create(ui.screen_wifi);
    lv_obj_set_size(card, 400, 190);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111122), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00FFAA), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Network row (dynamic — updates on connect)
    s_wifi_network_label = lv_label_create(card);
    lv_label_set_text(s_wifi_network_label, "Network:   —");
    lv_obj_set_style_text_font(s_wifi_network_label, &iosevka_16, 0);
    lv_obj_set_style_text_color(s_wifi_network_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(s_wifi_network_label, LV_ALIGN_TOP_LEFT, 15, 20);

    // Address row (dynamic — shows http://IP when server is running)
    s_wifi_address_label = lv_label_create(card);
    lv_label_set_text(s_wifi_address_label, "Address:   —");
    lv_obj_set_style_text_font(s_wifi_address_label, &iosevka_16, 0);
    lv_obj_set_style_text_color(s_wifi_address_label, lv_color_hex(0x66CCFF), 0);
    lv_obj_align(s_wifi_address_label, LV_ALIGN_TOP_LEFT, 15, 58);

    // Divider
    lv_obj_t* divider = lv_obj_create(card);
    lv_obj_set_size(divider, 370, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 15, 98);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x333355), 0);
    lv_obj_set_style_border_width(divider, 0, 0);

    // Instruction / status note (dynamic)
    s_wifi_note_label = lv_label_create(card);
    lv_label_set_text(s_wifi_note_label, "Tap Scan to choose a network");
    lv_obj_set_style_text_font(s_wifi_note_label, &iosevka_16, 0);
    lv_obj_set_style_text_color(s_wifi_note_label, lv_color_hex(0x888888), 0);
    lv_label_set_long_mode(s_wifi_note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_note_label, 370);
    lv_obj_align(s_wifi_note_label, LV_ALIGN_TOP_LEFT, 15, 113);

    // ── Forget network button (hidden until connected) ─────────────────────
    s_wifi_forget_btn = lv_btn_create(ui.screen_wifi);
    lv_obj_set_size(s_wifi_forget_btn, 200, 44);
    lv_obj_align(s_wifi_forget_btn, LV_ALIGN_BOTTOM_MID, 0, -118);
    lv_obj_set_style_bg_color(s_wifi_forget_btn, lv_color_hex(0x882222), 0);
    lv_obj_add_event_cb(s_wifi_forget_btn, [](lv_event_t*) {
        wifi_manager::disconnect(true);
        if (s_wifi_network_label) {
            lv_label_set_text(s_wifi_network_label, "Network:   —");
            lv_obj_set_style_text_color(s_wifi_network_label, lv_color_hex(0xAAAAAA), 0);
        }
        if (s_wifi_address_label) lv_label_set_text(s_wifi_address_label, "Address:   —");
        if (s_wifi_note_label)    lv_label_set_text(s_wifi_note_label, "Tap Scan to choose a network");
        if (s_wifi_forget_btn)    lv_obj_add_flag(s_wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_forget = lv_label_create(s_wifi_forget_btn);
    lv_label_set_text(lbl_forget, "Forget Network");
    lv_obj_set_style_text_font(lbl_forget, &iosevka_16, 0);
    lv_obj_center(lbl_forget);
    lv_obj_add_flag(s_wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);

    // ── Scan button (bottom-left) ──────────────────────────────────────────
    lv_obj_t* btn_scan = lv_btn_create(ui.screen_wifi);
    lv_obj_set_size(btn_scan, 165, 50);
    lv_obj_align(btn_scan, LV_ALIGN_BOTTOM_MID, -90, -58);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x0055AA), 0);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x0077CC), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_scan, [](lv_event_t*) {
        wifiShowNetworkModal();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_scan = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan, "Scan");
    lv_obj_set_style_text_font(lbl_scan, &iosevka_16, 0);
    lv_obj_center(lbl_scan);

    // ── Reboot to Radar button (bottom-right) ─────────────────────────────
    lv_obj_t* btn_reboot = lv_btn_create(ui.screen_wifi);
    lv_obj_set_size(btn_reboot, 165, 50);
    lv_obj_align(btn_reboot, LV_ALIGN_BOTTOM_MID, 90, -58);
    lv_obj_set_style_bg_color(btn_reboot, lv_color_hex(0xFF6600), 0);
    lv_obj_set_style_bg_color(btn_reboot, lv_color_hex(0xFF8800), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_reboot, [](lv_event_t*) {
        Serial.println("[WIFI_SCREEN] Reboot to Radar tapped");
        settings_manager::saveWiFiSTABoot(false);
        esp_restart();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_reboot = lv_label_create(btn_reboot);
    lv_label_set_text(lbl_reboot, "Reboot");
    lv_obj_set_style_text_font(lbl_reboot, &iosevka_16, 0);
    lv_obj_center(lbl_reboot);

    // ── 2s status timer ───────────────────────────────────────────────────
    lv_timer_create([](lv_timer_t*) {
        if (!s_wifi_network_label) return;
        auto cs = wifi_manager::getConnectionStatus();

        if (cs == wifi_manager::ConnectionStatus::CONNECTED) {
            String ip   = wifi_manager::getIPAddress();
            String ssid = wifi_manager::getSSID();

            // Auto-start GPX server on first connection — Network Task is not running
            // in WiFi STA boot mode (minimal_boot), so the Network Task's auto-start
            // logic never fires. gpx_server::start() with an active STA connection
            // skips AP creation and binds httpd to the STA IP directly.
            if (!gpx_server::isRunning() && ip != "0.0.0.0" && !ip.isEmpty()) {
                if (gpx_server::start()) {
                    Serial.printf("[WIFI_SCREEN] GPX server started at http://%s\n", ip.c_str());
                }
            }

            char buf[64];
            snprintf(buf, sizeof(buf), "Network:   %s", ssid.c_str());
            lv_label_set_text(s_wifi_network_label, buf);
            lv_obj_set_style_text_color(s_wifi_network_label, lv_color_white(), 0);

            if (s_wifi_address_label) {
                if (gpx_server::isRunning()) {
                    snprintf(buf, sizeof(buf), "Address:   http://%s", ip.c_str());
                    lv_obj_set_style_text_color(s_wifi_address_label, lv_color_hex(0x66CCFF), 0);
                } else {
                    snprintf(buf, sizeof(buf), "Address:   %s", ip.c_str());
                    lv_obj_set_style_text_color(s_wifi_address_label, lv_color_hex(0x888888), 0);
                }
                lv_label_set_text(s_wifi_address_label, buf);
            }

            if (s_wifi_note_label) {
                lv_label_set_text(s_wifi_note_label,
                    gpx_server::isRunning()
                        ? "Open browser and navigate to the address above"
                        : "GPX server starting...");
            }

            if (s_wifi_forget_btn) lv_obj_clear_flag(s_wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);

        } else if (cs == wifi_manager::ConnectionStatus::CONNECTING ||
                   cs == wifi_manager::ConnectionStatus::RECONNECTING) {
            lv_label_set_text(s_wifi_network_label, "Network:   Connecting...");
            lv_obj_set_style_text_color(s_wifi_network_label, lv_color_hex(0xFFAA00), 0);
            if (s_wifi_address_label) lv_label_set_text(s_wifi_address_label, "Address:   —");
            if (s_wifi_note_label)    lv_label_set_text(s_wifi_note_label, "Waiting for connection...");
            if (s_wifi_forget_btn)    lv_obj_add_flag(s_wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);

        } else {
            lv_label_set_text(s_wifi_network_label, "Network:   Not connected");
            lv_obj_set_style_text_color(s_wifi_network_label, lv_color_hex(0xAAAAAA), 0);
            if (s_wifi_address_label) lv_label_set_text(s_wifi_address_label, "Address:   —");
            if (s_wifi_note_label)    lv_label_set_text(s_wifi_note_label, "Tap Scan to choose a network");
            if (s_wifi_forget_btn)    lv_obj_add_flag(s_wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }, 2000, nullptr);

    Serial.println("[UI] WiFi STA mode screen created");
}

} // namespace ui_manager