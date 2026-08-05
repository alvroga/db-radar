#include "navigation.h"
#include "system_config.h"
#include "backlight.h"
#include "scanner.h"
#include "rtc_pcf85063.h"
#include "diagnostics.h"
#include "device_manager.h"
#include "memory_manager.h"
#include "task_manager.h"
#include "settings_manager.h"
#include "ui_manager.h"
#include "ui/dev_screen.h"
#include "ui/waypoint_screen.h"
#include "hardware/connectivity/beacon_proximity.h"
#include "hardware/buzzer.h"
#include "hardware/sensors/compass_qmc5883l.h"
#include "hardware/i2c/i2c_manager.h"
#include "gpx/gpx_loader.h"
#include "core/arduino_compat.h"
#include <cfloat>
#include <cmath>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace navigation {

// =============================================================================
// DAYLIGHT MODE COLOR SCHEME
// =============================================================================
// High contrast colors for outdoor visibility when backlight can't compete
// with sunlight. Uses light background with dark elements.

struct ColorScheme {
    uint32_t background;         // Radar background
    uint32_t grid;               // Grid lines
    uint32_t waypoint;           // Waypoint circles
    uint32_t waypoint_glow;      // Waypoint glow (disabled in daylight)
    uint32_t offscreen_indicator;// Off-screen triangles
    uint32_t center_triangle;    // User position marker
    uint32_t north_indicator;    // North "N" circle
    bool enable_glow;            // Enable glow effects
};

// Normal mode: Dark green background, bright elements
static const ColorScheme COLOR_NORMAL = {
    .background = 0x3A9949,       // Dark green
    .grid = 0x000000,             // Black
    .waypoint = 0xFFFF00,         // Yellow
    .waypoint_glow = 0xFFFF88,    // Soft yellow-white
    .offscreen_indicator = 0xFF8800,  // Orange
    .center_triangle = 0xD43701,  // Red
    .north_indicator = 0xFF0000,  // Red
    .enable_glow = true
};

// Daylight mode: Bright background, dark elements for maximum contrast
static const ColorScheme COLOR_DAYLIGHT = {
    .background = 0xB8F0B8,       // Light green with more saturation (visible green tint)
    .grid = 0x000000,             // Black (high contrast)
    .waypoint = 0xCC5500,         // Dark orange (visible on light bg)
    .waypoint_glow = 0xFF8800,    // Orange glow
    .offscreen_indicator = 0x000080,  // Dark blue (arrows pointing to off-screen waypoints)
    .center_triangle = 0xCC0000,  // Dark red (still visible)
    .north_indicator = 0xCC0000,  // Dark red
    .enable_glow = true           // Keep glow enabled
};

// Cache daylight mode state to avoid repeated NVS reads
static bool g_daylight_mode = false;
static uint32_t g_daylight_mode_last_check = 0;
static const uint32_t DAYLIGHT_CHECK_INTERVAL_MS = 1000;  // Check every 1 second

static const ColorScheme& getColorScheme() {
    // Periodically refresh daylight mode from cached settings (no I/O)
    uint32_t now = millis();
    if (now - g_daylight_mode_last_check > DAYLIGHT_CHECK_INTERVAL_MS) {
        const auto& settings = settings_manager::getSettings();
        if (g_daylight_mode != settings.daylight_mode) {
            g_daylight_mode = settings.daylight_mode;
            Serial.printf("[RADAR] Color scheme: %s\n",
                         g_daylight_mode ? "DAYLIGHT (high contrast)" : "NORMAL");
            // Update UI elements for daylight mode (shadow, HUD backgrounds)
            ui_manager::updateDaylightMode(g_daylight_mode);
        }
        g_daylight_mode_last_check = now;
    }

    return g_daylight_mode ? COLOR_DAYLIGHT : COLOR_NORMAL;
}

// Global state
static NavState g_nav_state;

NavState& getNavState() {
    return g_nav_state;
}

bool init() {
    // Initialize navigation state
    g_nav_state.touch_x = 240;
    g_nav_state.touch_y = 240;
    g_nav_state.touch_pressed = false;
    g_nav_state.flush_count = 0;
    g_nav_state.first_frame_done = false;

    Serial.println("[NAVIGATION] Initialized successfully");
    return true;
}

// Utility functions - screen navigation
void goToMainScreen() {
    goToRadarScreen();  // Radar is the main screen
}

void goToRadarScreen() {
    // WiFi upload mode: radar is disabled, stay in settings
    if (settings_manager::getSettings().wifi_enabled) {
        Serial.println("[NAV] WiFi mode active — radar access blocked");
        return;
    }

    ui_manager::UIState& ui = ui_manager::getUIState();

    // Load radar screen FIRST — LVGL must always have a valid active screen.
    // Deleting the current screen before loading the next one leaves LVGL
    // with no active screen and crashes on the next render tick.
    if (ui.screen_radar && lv_obj_is_valid(ui.screen_radar)) {
        lv_scr_load(ui.screen_radar);
        Serial.println("[NAVIGATION] Switched to radar screen");
    }

    // Now safe to delete the waypoint screen (radar is already active)
    waypoint_screen::close();

    // Refresh radar
    if (ui.radar_obj && lv_obj_is_valid(ui.radar_obj)) {
        Serial.println("[NAVIGATION] Radar surface valid - updating display");
        updateRadarDisplay();
    } else {
        Serial.println("[NAVIGATION] WARNING: Radar surface invalid - skipping display update");
    }
}

void goToSettingsScreen() {
    ui_manager::UIState& ui = ui_manager::getUIState();

    // Three failure modes have been observed here (docs/crash.md, 2026-08-01/02), and
    // they all trace to one rule: LVGL must always have a valid active screen, AND at
    // most one full settings tree (~6 tabs of widgets) may exist at a time. LV_MEM_SIZE
    // is only 64KB (include/ui/lv_conf.h) and LVGL doesn't check allocation failures at
    // every call site (e.g. the unchecked lv_mem_realloc() growing disp->screens[],
    // lv_obj_class.c:70), so violating either half of that rule corrupts LVGL state —
    // sometimes as an immediate near-null crash (lv_label_create during DEV tab
    // creation), sometimes as a corrupted allocator free-list that spins forever on the
    // next lv_mem_alloc() (a hang, not a crash — UI_Task loop count freezes with no
    // panic). An earlier version of this function tried to satisfy both halves by
    // deferring the delete only when the outgoing screen was active, but that still
    // built the replacement tree while the active one was alive — reproducing the
    // "two full trees alive" condition on that path, just with no crash-log evidence
    // that time. Fix: never build a replacement while an old settings tree is alive,
    // even when it's the active screen — bounce off the always-valid radar screen as a
    // momentary placeholder first. This never flushes to the physical panel: nothing
    // between the two lv_scr_load() calls below calls lv_task_handler(), so LVGL only
    // marks radar dirty and then immediately re-marks the new settings screen dirty
    // before the next real refresh cycle runs.
    lv_obj_t* old_screen = ui.screen_settings;
    if (old_screen && lv_obj_is_valid(old_screen)) {
        if (lv_scr_act() == old_screen && ui.screen_radar && lv_obj_is_valid(ui.screen_radar)) {
            lv_scr_load(ui.screen_radar);
        }
        lv_obj_del(old_screen);
        old_screen = nullptr;
    }

    // Always recreate to pick up fresh runtime state (Found/Missing, WiFi status,
    // etc.) — settings_screen::create() no-ops if ui.screen_settings is non-null,
    // so it must be cleared here regardless of what happened to old_screen above.
    ui.screen_settings      = nullptr;
    ui.settings_tabview     = nullptr;
    ui.settings_tab_gps     = nullptr;
    ui.settings_tab_wifi    = nullptr;
    ui.settings_tab_display = nullptr;
    ui.settings_tab_sound   = nullptr;
    ui.settings_tab_beacon  = nullptr;
    ui.settings_tab_dev     = nullptr;

    Serial.println("[NAVIGATION] Creating settings screen...");
    ui_manager::createSettingsScreen();

    // Switch to settings screen
    if (ui.screen_settings && lv_obj_is_valid(ui.screen_settings)) {
        lv_scr_load(ui.screen_settings);
        Serial.println("[NAVIGATION] Switched to settings screen");
    } else {
        Serial.println("[NAVIGATION] ERROR: Settings screen is invalid!");
    }
}

void goToDevScreen() {
    Serial.println("[NAVIGATION] Navigating to DEV screen...");
    dev_screen::open();
}

void goToWaypointScreen() {
    Serial.println("[NAVIGATION] Navigating to waypoint detail screen...");
    waypoint_screen::open();
}

void showSettings() {
    // Alias for goToSettingsScreen (used by dev_screen back button)
    goToSettingsScreen();
}

// GPS coordinate conversion - Haversine-based
constexpr double EARTH_RADIUS_M = 6371000.0;  // Earth radius in meters
constexpr double M_PI_LOCAL = 3.14159265358979323846;  // Renamed to avoid Arduino.h conflict

float metersToPixels(float meters, float meters_per_pixel) {
    return meters / meters_per_pixel;
}

// Calculate shortest angular difference between two headings (handles 359°→1° wrap)
// Returns delta in range [-180, +180]
float shortestAngularDifference(float from_heading, float to_heading) {
    float delta = fmodf(to_heading - from_heading + 540.0f, 360.0f) - 180.0f;
    return delta;
}

// Smooth heading transition using exponential moving average with shortest path
float smoothHeading(float current_heading, float target_heading, float smoothing_factor) {
    float delta = shortestAngularDifference(current_heading, target_heading);
    float new_heading = fmodf(current_heading + (delta * smoothing_factor) + 360.0f, 360.0f);
    return new_heading;
}

// Rotate point around radar center based on heading
// heading: GPS course in degrees (0=North, 90=East, 180=South, 270=West)
// For heading-up mode: rotates map so heading points "up" (negative rotation)
void rotatePoint(int& screen_x, int& screen_y, float heading, int center_x, int center_y) {
    // Translate to origin (center of radar)
    int rel_x = screen_x - center_x;
    int rel_y = screen_y - center_y;

    // Rotate by -heading (counterclockwise) to make heading point "up"
    // In navigation: if heading is 90° (East), we rotate map -90° so East points up
    float angle_rad = -heading * M_PI_LOCAL / 180.0f;
    float cos_a = cos(angle_rad);
    float sin_a = sin(angle_rad);

    int rotated_x = (int)(rel_x * cos_a - rel_y * sin_a);
    int rotated_y = (int)(rel_x * sin_a + rel_y * cos_a);

    // Translate back
    screen_x = rotated_x + center_x;
    screen_y = rotated_y + center_y;
}

void latLonToScreen(double lat, double lon, int& x, int& y, int screen_size) {
    ui_manager::UIState& ui = ui_manager::getUIState();

    // Equirectangular approximation, not Haversine — accurate to well under a
    // pixel at radar scale (<= a few km). Only dx/dy are needed here (not
    // distance or bearing themselves), so this drops straight to 2 multiplies
    // with no sqrt/atan2 at all.
    double lat1 = ui.center_lat * M_PI_LOCAL / 180.0;
    double dLat = (lat - ui.center_lat) * M_PI_LOCAL / 180.0;
    double dLon = (lon - ui.center_lon) * M_PI_LOCAL / 180.0;

    // North is up (negative Y), East is right (positive X)
    double dx_meters = EARTH_RADIUS_M * dLon * cos(lat1);
    double dy_meters = -EARTH_RADIUS_M * dLat;  // Negative because screen Y increases downward

    // Convert meters to pixels using current zoom level
    float meters_per_pixel = ui.getMetersPerPixel(screen_size);
    float dx_pixels = metersToPixels(dx_meters, meters_per_pixel);
    float dy_pixels = metersToPixels(dy_meters, meters_per_pixel);

    // Center is middle of screen
    int center_x = screen_size / 2;
    int center_y = screen_size / 2;

    x = center_x + (int)dx_pixels;
    y = center_y + (int)dy_pixels;

    // Apply heading-up rotation if enabled
    if (ui.heading_up_mode && ui.current_heading != 0.0f) {
        rotatePoint(x, y, ui.current_heading, center_x, center_y);
    }
}

static void drawRadarGrid(lv_draw_ctx_t* ctx, int screen_size, int grid_spacing_pixels) {
    if (!ctx) return;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 3; //grid line width

    // Edge-aligned grid algorithm:
    // Always draw lines at exact intervals from 0, ensuring perfect edge alignment
    // Lines will be at: 0, grid_spacing, 2*grid_spacing, ..., screen_size-1

    // Draw vertical lines from x=0 to x=screen_size-1
    for (int x = 0; x < screen_size; x += grid_spacing_pixels) {
        lv_point_t points[2];
        points[0].x = x;
        points[0].y = 0;
        points[1].x = x;
        points[1].y = screen_size - 1;
        lv_draw_line(ctx, &line_dsc, &points[0], &points[1]);
    }

    // Always draw right edge line
    if ((screen_size - 1) % grid_spacing_pixels != 0) {
        lv_point_t points[2];
        points[0].x = screen_size - 1;
        points[0].y = 0;
        points[1].x = screen_size - 1;
        points[1].y = screen_size - 1;
        lv_draw_line(ctx, &line_dsc, &points[0], &points[1]);
    }

    // Draw horizontal lines from y=0 to y=screen_size-1
    for (int y = 0; y < screen_size; y += grid_spacing_pixels) {
        lv_point_t points[2];
        points[0].x = 0;
        points[0].y = y;
        points[1].x = screen_size - 1;
        points[1].y = y;
        lv_draw_line(ctx, &line_dsc, &points[0], &points[1]);
    }

    // Always draw bottom edge line
    if ((screen_size - 1) % grid_spacing_pixels != 0) {
        lv_point_t points[2];
        points[0].x = 0;
        points[0].y = screen_size - 1;
        points[1].x = screen_size - 1;
        points[1].y = screen_size - 1;
        lv_draw_line(ctx, &line_dsc, &points[0], &points[1]);
    }
}

static void drawCenterTriangle(lv_draw_ctx_t* ctx, int screen_size) {
    if (!ctx) return;

    // Get current color scheme
    const ColorScheme& colors = getColorScheme();

    // Equilateral triangle in center: 44x38px (44px sides, 38px height)
    int center_x = screen_size / 2;
    int center_y = screen_size / 2;

    // Calculate triangle vertices (pointing north/up)
    // Adjusted to center the triangle's visual mass (geometric centroid)
    int half_base = ui_manager::RadarConfig::CENTER_TRIANGLE_SIDE / 2;  // 22px
    int height = ui_manager::RadarConfig::CENTER_TRIANGLE_HEIGHT;       // 38px

    // Geometric center of triangle is 1/3 from base, so offset by height/3 DOWNWARD
    int centroid_offset = height / 3;  // ~13px

    lv_point_t points[3];
    points[0].x = center_x;                          // Top (north) - apex
    points[0].y = center_y - height / 2 - centroid_offset;  // Offset DOWN to center mass
    points[1].x = center_x - half_base;              // Bottom left
    points[1].y = center_y + height / 2 - centroid_offset;
    points[2].x = center_x + half_base;              // Bottom right
    points[2].y = center_y + height / 2 - centroid_offset;

    lv_draw_rect_dsc_t tri_dsc;
    lv_draw_rect_dsc_init(&tri_dsc);
    tri_dsc.bg_color = lv_color_hex(colors.center_triangle);
    tri_dsc.bg_opa = LV_OPA_COVER;

    lv_draw_polygon(ctx, &tri_dsc, points, 3);
}

static void drawNorthIndicator(lv_draw_ctx_t* ctx, int screen_size) {
    if (!ctx) return;

    ui_manager::UIState& ui = ui_manager::getUIState();

    // Only draw north indicator in heading-up mode and when enabled in settings
    if (!ui.heading_up_mode) return;
    if (!settings_manager::getSettings().north_indicator_enabled) return;

    // Get current color scheme
    const ColorScheme& colors = getColorScheme();

    int center_x = screen_size / 2;
    int center_y = screen_size / 2;

    // North indicator distance from center (near edge, but inside safe area)
    int north_distance = screen_size / 2 - 50;  // 50px from edge

    // Calculate where north is relative to current heading
    // If heading is 0° (north), north indicator is at top (angle = 0)
    // If heading is 90° (east), north indicator is at left (angle = -90° = 270°)
    // Rotation: -current_heading (same as map rotation)
    float north_angle = -ui.current_heading * M_PI_LOCAL / 180.0f;
    int north_x = center_x + (int)(north_distance * sin(north_angle));
    int north_y = center_y - (int)(north_distance * cos(north_angle));

    // Draw circle background (30px diameter)
    lv_draw_arc_dsc_t circle_dsc;
    lv_draw_arc_dsc_init(&circle_dsc);
    circle_dsc.color = lv_color_hex(colors.north_indicator);
    circle_dsc.width = 30;  // Thick arc to create filled circle effect
    circle_dsc.opa = LV_OPA_COVER;
    const lv_point_t north_center = { (lv_coord_t)north_x, (lv_coord_t)north_y };
    lv_draw_arc(ctx, &circle_dsc, &north_center, 15, 0, 360);

    // Draw "N" text (white in normal mode, white/yellow in daylight for contrast)
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_white();  // White "N" visible on red/dark red background
    label_dsc.font = &iosevka_16;

    // Center the "N" text (approximate offset for single character).
    // Bottom edge runs to the screen edge, matching what lv_canvas_draw_text did
    // (it set y2 to the canvas height, i.e. unbounded downward).
    const lv_area_t n_area = { (lv_coord_t)(north_x - 5), (lv_coord_t)(north_y - 7),
                               (lv_coord_t)(north_x - 5 + 20 - 1), (lv_coord_t)(screen_size - 1) };
    lv_draw_label(ctx, &label_dsc, &n_area, "N", NULL);
}

/**
 * @brief Draw proximity gauge arc on radar circle edge.
 *
 * Arc starts at 12 o'clock (top) and fills clockwise like a gauge.
 * Small arc = far away. Full circle = right next to you.
 * Color: light green (normal mode) / dark cyan (daylight mode).
 *
 * Priority: fixed waypoint (GPS distance) > beacon proximity (RSSI).
 * - Waypoint: 100m → 0° (hidden), 0m → 355° (full).
 * - Beacon:  -90 dBm → 0° (hidden), -45 dBm → 355° (full).
 *
 * Capped at 355° to avoid LVGL drawing nothing when start == end at 360°.
 */
static void drawBeaconProximityGauge(lv_draw_ctx_t* ctx, int screen_size) {
    if (!ctx) return;

    const int cx = screen_size / 2;
    const int cy = screen_size / 2;

    // ── Beacon proximity: full 360° ring, width grows by zone ──
    // Fixed waypoint proximity is shown as a star on the waypoint dot (drawWaypoints).
    // Ring sits at the circular display edge and grows inward as signal strengthens.
    // Zone is hysteresis-confirmed so ring width is stable (no flicker).
    {
        beacon_proximity::BeaconState state = beacon_proximity::getState();
        if (!state.scanning_enabled) return;
        if (beacon_proximity::isFound()) return;  // Found — no visuals, radar is clear

        if (state.zone == beacon_proximity::ProximityZone::CLOSE) {
            // CLOSE: solid blue fill using rect+LV_RADIUS_CIRCLE — matches the hardware
            // circular boundary exactly, no gap possible (arc-based fill left a gap)
            lv_draw_rect_dsc_t fill_dsc;
            lv_draw_rect_dsc_init(&fill_dsc);
            fill_dsc.bg_color     = g_daylight_mode ? lv_color_hex(0x1565C0) : lv_color_hex(0x4488FF);
            fill_dsc.bg_opa       = LV_OPA_COVER;
            fill_dsc.radius       = LV_RADIUS_CIRCLE;
            fill_dsc.border_width = 0;
            const lv_area_t full_area = { 0, 0, (lv_coord_t)(screen_size - 1), (lv_coord_t)(screen_size - 1) };
            lv_draw_rect(ctx, &fill_dsc, &full_area);

            // Orange ball at center
            const int ball_r = 60;
            lv_draw_rect_dsc_t ball_dsc;
            lv_draw_rect_dsc_init(&ball_dsc);
            ball_dsc.bg_color     = lv_color_hex(0xFF6B00);
            ball_dsc.bg_opa       = LV_OPA_COVER;
            ball_dsc.radius       = LV_RADIUS_CIRCLE;
            ball_dsc.border_width = 0;
            const lv_area_t ball_area = { (lv_coord_t)(cx - ball_r), (lv_coord_t)(cy - ball_r),
                                          (lv_coord_t)(cx + ball_r - 1), (lv_coord_t)(cy + ball_r - 1) };
            lv_draw_rect(ctx, &ball_dsc, &ball_area);

            // Filled star: 5 arm triangles + center pentagon, all convex (safe for LVGL)
            const float STAR_OUTER = 22.0f;
            const float STAR_INNER = 9.0f;
            lv_point_t s_outer[5], s_inner[5];
            for (int i = 0; i < 5; i++) {
                float ao = ((float)i * 72.0f - 90.0f) * (float)M_PI_LOCAL / 180.0f;
                float ai = ((float)i * 72.0f - 54.0f) * (float)M_PI_LOCAL / 180.0f;
                s_outer[i].x = (lv_coord_t)(cx + STAR_OUTER * cosf(ao));
                s_outer[i].y = (lv_coord_t)(cy + STAR_OUTER * sinf(ao));
                s_inner[i].x = (lv_coord_t)(cx + STAR_INNER * cosf(ai));
                s_inner[i].y = (lv_coord_t)(cy + STAR_INNER * sinf(ai));
            }
            lv_draw_rect_dsc_t star_dsc;
            lv_draw_rect_dsc_init(&star_dsc);
            star_dsc.bg_color     = lv_color_hex(0x8B0000);
            star_dsc.bg_opa       = LV_OPA_COVER;
            star_dsc.border_width = 0;
            // 5 arm triangles (each convex: tip + two adjacent inner points)
            for (int i = 0; i < 5; i++) {
                lv_point_t arm[3] = { s_outer[i], s_inner[(i + 4) % 5], s_inner[i] };
                lv_draw_polygon(ctx, &star_dsc, arm, 3);
            }
            // Center pentagon (convex)
            lv_draw_polygon(ctx, &star_dsc, s_inner, 5);
            return;
        }

        // VERY_FAR / FAR / MEDIUM: ring flush with screen edge, grows inward as the
        // signal strengthens. radius = 239 - width/2 keeps the outer edge at 239px
        // regardless of thickness.
        //
        // §7.3c: width is **continuous** in rssi_display, not four discrete steps.
        // rssi_display is the slow (τ=1.0s) EMA and had been computed every sample
        // and read by nothing at all — the ring was quantised to the zone, so it
        // sat still through most of an approach and then jumped. Same principle as
        // the waypoint sonar tempo: continuous for the analogue quantity, and
        // hysteresis kept only where a genuinely discrete decision is made — which
        // here is the two decisions *around* the ring, both still zone-gated:
        // whether to draw one at all (OUT_OF_RANGE), and whether to switch to the
        // solid CLOSE fill handled above.
        if (state.zone == beacon_proximity::ProximityZone::OUT_OF_RANGE) return;

        constexpr float RING_RSSI_MIN = -90.0f;  // ring at its thinnest at/below this
        constexpr float RING_RSSI_MAX = -65.0f;  // ring at its thickest at/above this (CLOSE boundary)
        constexpr float RING_W_MIN    = 6.0f;
        constexpr float RING_W_MAX    = 34.0f;

        float t = (state.rssi_display - RING_RSSI_MIN) / (RING_RSSI_MAX - RING_RSSI_MIN);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const int32_t ring_width = (int32_t)(RING_W_MIN + t * (RING_W_MAX - RING_W_MIN) + 0.5f);

        lv_draw_arc_dsc_t arc_dsc;
        lv_draw_arc_dsc_init(&arc_dsc);
        arc_dsc.color = g_daylight_mode ? lv_color_hex(0x1565C0) : lv_color_hex(0x4488FF);
        arc_dsc.width = ring_width;
        arc_dsc.opa   = LV_OPA_COVER;
        int32_t ring_radius = 239 - ring_width / 2;  // outer edge always at 239px
        const lv_point_t ring_center = { (lv_coord_t)cx, (lv_coord_t)cy };
        lv_draw_arc(ctx, &arc_dsc, &ring_center, (uint16_t)ring_radius, 0, 360);
    }
}

static void drawOffScreenIndicator(lv_draw_ctx_t* ctx, double bearing, int screen_size) {
    if (!ctx) return;

    int center_x = screen_size / 2;
    int center_y = screen_size / 2;
    int radius = screen_size / 2;

    // Use configuration constants
    int inset = ui_manager::RadarConfig::INDICATOR_EDGE_INSET;
    int tri_size = ui_manager::RadarConfig::INDICATOR_SIZE;
    int border_width = ui_manager::RadarConfig::INDICATOR_BORDER_WIDTH;

    float edge_x = center_x + (radius - inset) * sin(bearing);
    float edge_y = center_y - (radius - inset) * cos(bearing);

    // Calculate triangle vertices pointing toward waypoint direction
    // Base width doubled horizontally for better visibility
    lv_point_t points[3];

    // Point 0: tip pointing outward (toward waypoint)
    points[0].x = edge_x + (tri_size * 0.8) * sin(bearing);
    points[0].y = edge_y - (tri_size * 0.8) * cos(bearing);

    // Point 1: left base (wider angle for double-width base)
    float base_distance_left = tri_size * 0.5;
    // Double the perpendicular distance from center line
    float perpendicular_left = base_distance_left * sin(M_PI_LOCAL * 2.5 / 3.0) * 2.0;
    float parallel_left = base_distance_left * cos(M_PI_LOCAL * 2.5 / 3.0);
    points[1].x = edge_x + parallel_left * sin(bearing) + perpendicular_left * cos(bearing);
    points[1].y = edge_y - parallel_left * cos(bearing) + perpendicular_left * sin(bearing);

    // Point 2: right base (wider angle for double-width base)
    float base_distance_right = tri_size * 0.5;
    // Double the perpendicular distance from center line
    float perpendicular_right = base_distance_right * sin(M_PI_LOCAL * 2.5 / 3.0) * 2.0;
    float parallel_right = base_distance_right * cos(M_PI_LOCAL * 2.5 / 3.0);
    points[2].x = edge_x + parallel_right * sin(bearing) - perpendicular_right * cos(bearing);
    points[2].y = edge_y - parallel_right * cos(bearing) - perpendicular_right * sin(bearing);

    // Get current color scheme
    const ColorScheme& colors = getColorScheme();

    // Draw black border for better visibility
    lv_draw_rect_dsc_t border_dsc;
    lv_draw_rect_dsc_init(&border_dsc);
    border_dsc.bg_opa = LV_OPA_TRANSP;
    border_dsc.border_color = lv_color_black();
    border_dsc.border_width = border_width;
    border_dsc.border_opa = LV_OPA_COVER;
    lv_draw_polygon(ctx, &border_dsc, points, 3);

    // Draw triangle fill (color depends on mode)
    lv_draw_rect_dsc_t tri_dsc;
    lv_draw_rect_dsc_init(&tri_dsc);
    tri_dsc.bg_color = lv_color_hex(colors.offscreen_indicator);
    tri_dsc.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(ctx, &tri_dsc, points, 3);
}

static void drawWaypoints(lv_draw_ctx_t* ctx, int screen_size) {
    if (!ctx) return;

    ui_manager::UIState& ui = ui_manager::getUIState();

    // Calculate maximum relevant distance based on current zoom level
    // Shows waypoints within 10× zoom radius (e.g., 10m zoom shows up to 100m)
    int zoom_idx = static_cast<int>(ui.current_zoom);
    float zoom_radius = ui_manager::RadarConfig::ZOOM_CONFIGS[zoom_idx].radius_meters;
    float max_indicator_distance = zoom_radius * ui_manager::RadarConfig::DISTANCE_FILTER_MULTIPLIER;

    // Sector-based clustering for off-screen indicators
    // Divides edge into 8 sectors (N, NE, E, SE, S, SW, W, NW) at 45° each
    const int NUM_SECTORS = ui_manager::RadarConfig::INDICATOR_SECTORS;
    struct SectorWaypoint {
        bool has_waypoint = false;
        float closest_distance = FLT_MAX;
        double bearing = 0.0;
    };
    SectorWaypoint sectors[NUM_SECTORS];

    // Get current color scheme (normal or daylight mode)
    const ColorScheme& colors = getColorScheme();

    // Prepare drawing descriptor for on-screen beacons
    lv_draw_rect_dsc_t circle_dsc;
    lv_draw_rect_dsc_init(&circle_dsc);
    circle_dsc.bg_color = lv_color_hex(colors.waypoint);
    circle_dsc.bg_opa = LV_OPA_COVER;
    circle_dsc.radius = LV_RADIUS_CIRCLE;

    // Glow removed — was 18px shadow blur × 50 waypoints = dominant render cost (~40-60ms)
    circle_dsc.shadow_width = 0;
    if (!colors.enable_glow) {
        // Daylight mode: black border for contrast
        circle_dsc.border_color = lv_color_black();
        circle_dsc.border_width = 2;
        circle_dsc.border_opa = LV_OPA_COVER;
    }

    // Get current meters per pixel from zoom level
    float meters_per_pixel = ui.getMetersPerPixel(screen_size);

    // Pre-compute user's lat in radians — constant for all waypoints this frame
    double lat1      = ui.center_lat * M_PI_LOCAL / 180.0;
    double cos_lat1  = cos(lat1);

    // Track fixed waypoint if it ends up off-screen (drawn separately, bypasses clustering)
    bool fixed_off_screen = false;
    double fixed_off_bearing = 0.0;

    // Process all waypoints
    for (int i = 0; i < ui.waypoint_count; i++) {
        if (!ui.waypoints[i].valid) continue;
        // When a waypoint is fixed, render only that target — everything else is noise.
        // Eliminates N Haversine calcs, N polygon draws, and all sector clustering.
        if (ui.fixed_waypoint_index >= 0 && i != ui.fixed_waypoint_index) continue;

        // Convert lat/lon to screen coordinates using current zoom.
        // Equirectangular approximation, not Haversine — accurate to well
        // under a pixel at radar scale (<= a few km). Replaces 10 double
        // transcendental calls/waypoint with 2 multiplies + 1 sqrtf, plus a
        // single atan2f only for waypoints that end up off-screen (bearing
        // is otherwise unused).
        int x, y;
        double dLat = (ui.waypoints[i].lat - ui.center_lat) * M_PI_LOCAL / 180.0;
        double dLon = (ui.waypoints[i].lon - ui.center_lon) * M_PI_LOCAL / 180.0;

        // North is up (negative Y), East is right (positive X) — reuses cos_lat1
        float dx_meters = (float)(EARTH_RADIUS_M * dLon * cos_lat1);
        float dy_meters = (float)(-EARTH_RADIUS_M * dLat);
        float distance = sqrtf(dx_meters * dx_meters + dy_meters * dy_meters);

        // STRATEGY 1: Distance filtering - skip waypoints beyond maximum relevant distance
        // This eliminates waypoints thousands of km away
        if (distance > max_indicator_distance) {
            continue;  // Too far away, not relevant to current navigation
        }

        float dx_pixels = dx_meters / meters_per_pixel;
        float dy_pixels = dy_meters / meters_per_pixel;

        int center_x = screen_size / 2;
        int center_y = screen_size / 2;

        x = center_x + (int)dx_pixels;
        y = center_y + (int)dy_pixels;

        // Apply heading-up rotation if enabled (CRITICAL: rotate waypoints with heading!)
        if (ui.heading_up_mode && ui.current_heading != 0.0f) {
            rotatePoint(x, y, ui.current_heading, center_x, center_y);
        }

        // Check if waypoint is on-screen or off-screen
        if (x >= 0 && x < screen_size && y >= 0 && y < screen_size) {
            // On-screen: draw yellow circle beacon
            int size = ui_manager::RadarConfig::WAYPOINT_SIZE;  // 25x25
            int half_size = size / 2;
            const lv_area_t dot_area = { (lv_coord_t)(x - half_size), (lv_coord_t)(y - half_size),
                                         (lv_coord_t)(x - half_size + size - 1),
                                         (lv_coord_t)(y - half_size + size - 1) };
            lv_draw_rect(ctx, &circle_dsc, &dot_area);

            // Proximity star: drawn on top of the dot as you approach the fixed waypoint.
            // Three zone sizes so GPS jitter (±5m) won't cause rapid size flickering.
            // Reuses the same 5-arm decomposition as the beacon CLOSE star.
            if (i == ui.fixed_waypoint_index && distance < 50.0) {
                float OUTER, INNER;
                uint32_t star_color;
                if (distance < 10.0) {
                    OUTER = 22.0f; INNER = 9.0f;
                    star_color = ui.waypoints[i].found ? 0x00FF88u : 0x00FFCCu;  // Green if found, cyan if zone
                } else if (distance < 25.0) {
                    OUTER = 17.0f; INNER = 7.0f; star_color = 0xFFDD00u;  // Yellow: very close
                } else {
                    OUTER = 13.0f; INNER = 5.0f; star_color = 0xFF8800u;  // Orange: approaching
                }
                lv_point_t sp_outer[5], sp_inner[5];
                for (int k = 0; k < 5; k++) {
                    float ao = ((float)k * 72.0f - 90.0f) * (float)M_PI_LOCAL / 180.0f;
                    float ai = ((float)k * 72.0f - 54.0f) * (float)M_PI_LOCAL / 180.0f;
                    sp_outer[k].x = (lv_coord_t)(x + OUTER * cosf(ao));
                    sp_outer[k].y = (lv_coord_t)(y + OUTER * sinf(ao));
                    sp_inner[k].x = (lv_coord_t)(x + INNER * cosf(ai));
                    sp_inner[k].y = (lv_coord_t)(y + INNER * sinf(ai));
                }
                lv_draw_rect_dsc_t star_dsc;
                lv_draw_rect_dsc_init(&star_dsc);
                star_dsc.bg_color     = lv_color_hex(star_color);
                star_dsc.bg_opa       = LV_OPA_COVER;
                star_dsc.border_width = 0;
                for (int k = 0; k < 5; k++) {
                    lv_point_t arm[3] = { sp_outer[k], sp_inner[(k + 4) % 5], sp_inner[k] };
                    lv_draw_polygon(ctx, &star_dsc, arm, 3);
                }
                lv_draw_polygon(ctx, &star_dsc, sp_inner, 5);
            }
        } else {
            // Bearing is only needed off-screen (on-screen draw never uses it) — computed
            // here, not unconditionally, per waypoint. atan2f(east, north): east = dx_meters,
            // north = -dy_meters (dy_meters is already screen-sign, i.e. negated north).
            float bearing = atan2f(dx_meters, -dy_meters);
            if (i == ui.fixed_waypoint_index) {
                // Fixed waypoint off-screen: record bearing, draw separately with pulse
                fixed_off_screen = true;
                fixed_off_bearing = bearing;
            } else {
                // Off-screen: STRATEGY 2 - Sector clustering
                // Bearing: -π to π, convert to 0-360 degrees
                float bearing_deg = bearing * 180.0f / (float)M_PI_LOCAL;
                if (bearing_deg < 0) bearing_deg += 360.0f;

                // Calculate sector: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
                int sector = (int)((bearing_deg + 22.5f) / 45.0f) % NUM_SECTORS;

                // Keep only closest waypoint per sector
                if (distance < sectors[sector].closest_distance) {
                    sectors[sector].has_waypoint = true;
                    sectors[sector].closest_distance = distance;
                    sectors[sector].bearing = bearing;
                }
            }
        }
    }

    // Draw off-screen indicators: one per sector (maximum 8 indicators)
    // Apply heading-up rotation to bearing so indicators rotate with the compass
    for (int s = 0; s < NUM_SECTORS; s++) {
        if (sectors[s].has_waypoint) {
            double bearing = sectors[s].bearing;
            if (ui.heading_up_mode && ui.current_heading != 0.0f) {
                bearing -= ui.current_heading * M_PI_LOCAL / 180.0;
            }
            drawOffScreenIndicator(ctx, bearing, screen_size);
        }
    }

    // Draw fixed waypoint off-screen arrow separately — bypasses clustering
    if (fixed_off_screen) {
        double bearing = fixed_off_bearing;
        if (ui.heading_up_mode && ui.current_heading != 0.0f) {
            bearing -= ui.current_heading * M_PI_LOCAL / 180.0;
        }
        drawOffScreenIndicator(ctx, bearing, screen_size);
    }
}

// ── Waypoint sonar tempo ────────────────────────────────────────────────────
// Distance→tempo is a *continuous* geometric mapping. It used to be a four-zone
// ladder (5/5/20/20m wide) guarded by ±3m hysteresis and a 1s confirmation hold.
// That ladder worked — the guards did stop the tempo flipping at a boundary — but
// it was solving the wrong problem, and field testing said so:
//
//   * Not progressive. The 10-50m band, where most of an approach actually
//     happens, was two tempi. You walked 40m and heard one rate, then a step,
//     then one rate again. Approaching did not *sound* like approaching.
//   * Too busy far out. 30m already beeped at 750ms.
//
// A continuous tempo cannot flicker between rates — there are no rates to flicker
// between, only a glide — so the zone machinery, hysteresis and confirmation hold
// are all gone with it. What replaces them as the GPS-noise guard is a
// time-constant EMA on the *distance*, which is where the noise actually is:
// smooth the input, not the output.
//
// The mapping is geometric in both axes, i.e. equal distance *ratios* produce
// equal tempo *ratios* (halving the distance always doubles the tempo, wherever
// you are). Both distance-to-target and tempo are perceived that way, so a
// constant walking speed yields a steadily accelerating beat rather than one that
// does nothing for 20m and then lurches.

static constexpr float    WP_SONAR_MIN_M     =  2.0f;   // at/inside this, tempo floor
static constexpr float    WP_SONAR_MAX_M     = 50.0f;   // engage at/inside this (== 50m zoom radius)
static constexpr float    WP_SONAR_RELEASE_M = 55.0f;   // disengage beyond this
static constexpr uint32_t WP_SONAR_SLOW_MS   = 2000;    // interval at WP_SONAR_MAX_M
static constexpr uint32_t WP_SONAR_FAST_MS   =  250;    // interval at WP_SONAR_MIN_M

// Distance EMA time constant. 1.5s is a compromise: long enough to bury ±2-5m of
// good-fix GPS jitter, short enough that the ~2m of lag it adds at walking pace
// is well inside the GPS error it is filtering.
static constexpr float    WP_SONAR_TAU_S     = 1.5f;

/**
 * @brief Sonar interval for a smoothed distance, in ms. Never returns 0 —
 *        the engage/disengage decision is the caller's, not this function's.
 *
 * Yields 2000ms at 50m, ~1420 at 30m, ~1080 at 20m, ~707 at 10m, ~445 at 5m,
 * 250 at 2m and closer.
 */
static uint32_t waypointSonarInterval(float d) {
    if (d <= WP_SONAR_MIN_M) return WP_SONAR_FAST_MS;
    if (d >= WP_SONAR_MAX_M) return WP_SONAR_SLOW_MS;

    // t = fraction of the way along the log-distance span, 0 at MIN_M, 1 at MAX_M.
    const float t = logf(d / WP_SONAR_MIN_M) / logf(WP_SONAR_MAX_M / WP_SONAR_MIN_M);
    const float ratio = (float)WP_SONAR_SLOW_MS / (float)WP_SONAR_FAST_MS;
    return (uint32_t)((float)WP_SONAR_FAST_MS * powf(ratio, t) + 0.5f);
}

// Smoothed distance + the engage latch. Reset by resetWaypointSonarTempo()
// whenever the sonar is disengaged, so re-engaging never inherits a stale
// distance from a previous fix and glissandos in from the wrong tempo.
static float    s_wp_dist_ema_m    = -1.0f;   // < 0 = not yet initialised
static uint32_t s_wp_dist_last_ms  = 0;
static bool     s_wp_engaged       = false;

static void resetWaypointSonarTempo() {
    s_wp_dist_ema_m   = -1.0f;
    s_wp_dist_last_ms = 0;
    s_wp_engaged      = false;
}

/**
 * @brief Drive proximity sonar for a fixed (user-selected) waypoint.
 *
 * Maps GPS distance to sonar interval, taking sonar priority from beacon proximity.
 * Called every time updateRadarDisplay() runs (~10Hz with GPS lock).
 * Respects the master sound setting (button_sound_enabled).
 */
static void updateWaypointFixSonar() {
    ui_manager::UIState& ui = ui_manager::getUIState();

    if (ui.fixed_waypoint_index < 0 || ui.fixed_waypoint_index >= ui.waypoint_count) {
        beacon_proximity::suppressSonar(false);
        resetWaypointSonarTempo();
        return;
    }

    // ── Beacon has absolute priority ─────────────────────────────────────────
    //
    // A beacon is a thing you are trying to *find*; a fixed waypoint is an area
    // you are walking into, and its sonar is a secondary convenience. So the
    // moment the beacon is genuinely detected, the waypoint fix is **released**
    // outright rather than merely yielding the buzzer — leaving it fixed would
    // keep hiding every other waypoint from the radar and re-take the sonar as
    // soon as the beacon dipped out of range.
    //
    // Safe against flicker: isInRange() reads the *confirmed* zone, which needs
    // 1000 ms of hysteresis-gated agreement to enter, and OUT_OF_RANGE is below
    // -90 dBm — so this cannot fire on a single stray advertisement.
    if (beacon_proximity::isInRange()) {
        Serial.printf("[NAV] Fixed waypoint released — beacon detected (%s), beacon has priority\n",
                      beacon_proximity::zoneToString(beacon_proximity::getCurrentZone()));
        ui.fixed_waypoint_index = -1;
        beacon_proximity::suppressSonar(false);
        buzzer::stopSonar();   // beacon's own updateSonar() re-arms it on the next Network Task loop
        resetWaypointSonarTempo();
        return;
    }

    // Proximity sonar only active at 50m zoom (same as beacon proximity)
    if (ui.current_zoom != ui_manager::ZoomLevel::ZOOM_50M) {
        beacon_proximity::suppressSonar(false);
        buzzer::stopSonar();
        resetWaypointSonarTempo();
        return;
    }

    const auto& settings = settings_manager::getSettings();
    if (!settings.button_sound_enabled) {
        beacon_proximity::suppressSonar(false);
        resetWaypointSonarTempo();
        return;
    }

    const ui_manager::Waypoint& wp = ui.waypoints[ui.fixed_waypoint_index];
    if (!wp.valid || ui.center_lat == 0.0 || ui.center_lon == 0.0) {
        beacon_proximity::suppressSonar(false);
        resetWaypointSonarTempo();
        return;
    }

    // Arrival stop — the waypoint counterpart of the beacon's "found" silence.
    // Tapping the fixed waypoint within 15m already sets wp.found (handleTapAt),
    // exactly as tapping the beacon ball sets beacon_proximity's g_found. The
    // sonar simply never read the flag, so arriving gave you no way to shut it
    // up short of unfixing the waypoint or leaving 50m zoom. It reads it now.
    if (wp.found) {
        beacon_proximity::suppressSonar(false);
        buzzer::stopSonar();
        resetWaypointSonarTempo();
        return;
    }

    // Haversine distance to fixed waypoint
    double lat1 = ui.center_lat * M_PI_LOCAL / 180.0;
    double lat2 = wp.lat * M_PI_LOCAL / 180.0;
    double dLat = (wp.lat - ui.center_lat) * M_PI_LOCAL / 180.0;
    double dLon = (wp.lon - ui.center_lon) * M_PI_LOCAL / 180.0;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1) * cos(lat2) * sin(dLon / 2.0) * sin(dLon / 2.0);
    float distance_m = (float)(EARTH_RADIUS_M * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));

    // Smooth the distance before it drives anything. dt is measured rather than
    // assumed: this runs at the render-request rate, which is ~10Hz with a GPS fix
    // but is not guaranteed to be — so a sample-count-based EMA would silently
    // change its time constant whenever the render rate moved.
    const uint32_t now = millis();
    if (s_wp_dist_ema_m < 0.0f) {
        s_wp_dist_ema_m = distance_m;   // first sample: adopt, don't ramp in from 0
    } else {
        const float dt_s  = (float)(now - s_wp_dist_last_ms) / 1000.0f;
        const float alpha = 1.0f - expf(-dt_s / WP_SONAR_TAU_S);
        s_wp_dist_ema_m += alpha * (distance_m - s_wp_dist_ema_m);
    }
    s_wp_dist_last_ms = now;

    // Beeping-vs-silent is now the only discrete decision left in this path, so it
    // is the only one that still needs hysteresis: engage at 50m, release at 55m.
    // Without the gap, standing at the edge of range would stutter on and off.
    if (s_wp_engaged) {
        if (s_wp_dist_ema_m > WP_SONAR_RELEASE_M) s_wp_engaged = false;
    } else {
        if (s_wp_dist_ema_m <= WP_SONAR_MAX_M) s_wp_engaged = true;
    }

    if (!s_wp_engaged) {
        // Out of range — hand the buzzer back, the beacon may want it.
        beacon_proximity::suppressSonar(false);
        buzzer::stopSonar();
        return;
    }

    // Suppress beacon sonar so waypoint fix has priority.
    beacon_proximity::suppressSonar(true);

    // Safe to call every cycle with a drifting value: setSonarInterval() only
    // re-bases the beat grid when the sonar was not already active, so a gliding
    // tempo updates the period without resetting the phase.
    buzzer::setSonarInterval(waypointSonarInterval(s_wp_dist_ema_m), 30);
}

void drawBeaconFoundIndicator(lv_obj_t* canvas, bool daylight) {
    const int CX = 20, CY = 20;
    const lv_color_t color = daylight ? lv_color_black() : lv_color_white();

    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

    // Circle outline — 40×40 canvas, radius 17
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = color;
    arc_dsc.width = 3;
    arc_dsc.opa   = LV_OPA_COVER;
    lv_canvas_draw_arc(canvas, CX, CY, 17, 0, 360, &arc_dsc);

    // Solid 5-arm star — unchanged (OUTER=9, INNER=3.5), centered in the larger circle
    const float OUTER = 9.0f, INNER = 3.5f;
    lv_point_t sp_outer[5], sp_inner[5];
    for (int k = 0; k < 5; k++) {
        float ao = ((float)k * 72.0f - 90.0f) * (float)M_PI_LOCAL / 180.0f;
        float ai = ((float)k * 72.0f - 54.0f) * (float)M_PI_LOCAL / 180.0f;
        sp_outer[k].x = (lv_coord_t)(CX + OUTER * cosf(ao));
        sp_outer[k].y = (lv_coord_t)(CY + OUTER * sinf(ao));
        sp_inner[k].x = (lv_coord_t)(CX + INNER * cosf(ai));
        sp_inner[k].y = (lv_coord_t)(CY + INNER * sinf(ai));
    }
    lv_draw_rect_dsc_t star_dsc;
    lv_draw_rect_dsc_init(&star_dsc);
    star_dsc.bg_color     = color;
    star_dsc.bg_opa       = LV_OPA_COVER;
    star_dsc.border_width = 0;
    for (int k = 0; k < 5; k++) {
        lv_point_t arm[3] = { sp_outer[k], sp_inner[(k + 4) % 5], sp_inner[k] };
        lv_canvas_draw_polygon(canvas, arm, 3, &star_dsc);
    }
    lv_canvas_draw_polygon(canvas, sp_inner, 5, &star_dsc);
}

/**
 * @brief Paint the radar geometry directly into LVGL's draw buffer.
 *
 * Registered on the radar object for LV_EVENT_DRAW_MAIN, so it runs inside LVGL's
 * refresh with a live draw context. Nothing here may touch widgets or LVGL object
 * state — this is a draw pass, and invalidating from inside it would recurse.
 *
 * Replaces the old lv_canvas: painting into a canvas image meant LVGL then blitted
 * the whole 460 KB buffer into the draw buffer every refresh (~104 ms). Emitting
 * into the draw context skips that pass entirely.
 *
 * The object's background is painted by LVGL's own class handler before this runs
 * (see the style setup in ui_manager), which is why there is no fill step here.
 */
// Timestamp taken at LV_EVENT_DRAW_MAIN_BEGIN, i.e. before LVGL's class handler
// paints the object background. Subtracting it from the start of our DRAW_MAIN
// handler isolates the background fill, which is otherwise buried in the refresh.
static int64_t g_draw_begin_us = 0;

void radarDrawBeginCb(lv_event_t* e) {
    (void)e;
    g_draw_begin_us = esp_timer_get_time();
}

void radarDrawEventCb(lv_event_t* e) {
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
    if (!ctx) return;

    ui_manager::UIState& ui = ui_manager::getUIState();
    const int screen_size = system_config::display::SCREEN_WIDTH;

    const int64_t t_paint_start = esp_timer_get_time();

    // Background: painted by LVGL's class draw handler between DRAW_MAIN_BEGIN and
    // this callback, so the gap between those two is its cost (plus a negligible
    // event dispatch). Whatever refr_ms has left after bg + paint + rot + flush is
    // LVGL drawing things that are NOT the radar object — the screen and stage
    // backgrounds underneath it, and the HUD widgets on top.
    g_nav_state.bg_us = (g_draw_begin_us > 0)
                      ? (uint32_t)(t_paint_start - g_draw_begin_us)
                      : 0;

    // Draw grid (black lines) - perfectly aligned at all zoom levels
    const int grid_spacing_pixels = ui.getGridSpacingPixels(screen_size);
    const int64_t t_grid = esp_timer_get_time();
    drawRadarGrid(ctx, screen_size, grid_spacing_pixels);
    g_nav_state.grid_us = (uint32_t)(esp_timer_get_time() - t_grid);

    // Draw center triangle (red equilateral - always at center representing user)
    const int64_t t_tri = esp_timer_get_time();
    drawCenterTriangle(ctx, screen_size);
    g_nav_state.deco_us = (uint32_t)(esp_timer_get_time() - t_tri);

    // Draw waypoints (yellow circles) - they move as user moves
    // Drawn after triangle so they appear on top
    const int64_t t_wpt = esp_timer_get_time();
    drawWaypoints(ctx, screen_size);
    g_nav_state.wpt_us = (uint32_t)(esp_timer_get_time() - t_wpt);

    // Draw north indicator (shows where north is in heading-up mode)
    const int64_t t_deco2 = esp_timer_get_time();
    drawNorthIndicator(ctx, screen_size);

    // Draw beacon proximity gauge (arc grows from top as signal strengthens)
    // Drawn last so it overlays everything including the north indicator
    drawBeaconProximityGauge(ctx, screen_size);
    g_nav_state.deco_us += (uint32_t)(esp_timer_get_time() - t_deco2);

    g_nav_state.paint_us = (uint32_t)(esp_timer_get_time() - t_paint_start);
}

void updateRadarDisplay() {
    ui_manager::UIState& ui = ui_manager::getUIState();
    const device_manager::DeviceState& dev = device_manager::getDeviceState();

    // Comprehensive object validation before any operations
    if (!ui.radar_obj) {
        Serial.println("[RADAR] ERROR: radar_obj is NULL!");
        return;
    }

    if (!lv_obj_is_valid(ui.radar_obj)) {
        Serial.println("[RADAR] ERROR: radar_obj is invalid (possibly deleted)!");
        return;
    }

    // This function no longer paints — it refreshes the HUD label widgets and then
    // invalidates the radar object. The geometry is emitted by radarDrawEventCb
    // during LVGL's refresh, so its cost shows up inside refr_ms, not here.
    const int64_t t_label_start = esp_timer_get_time();

    // Update zoom level label — only when zoom actually changes
    if (ui.zoom_label && lv_obj_is_valid(ui.zoom_label)) {
        static ui_manager::ZoomLevel s_last_zoom = static_cast<ui_manager::ZoomLevel>(-1);
        if (ui.current_zoom != s_last_zoom) {
            const char* zoom_name = "";
            switch (ui.current_zoom) {
                case ui_manager::ZoomLevel::ZOOM_1KM:  zoom_name = "1km"; break;
                case ui_manager::ZoomLevel::ZOOM_500M: zoom_name = "500m"; break;
                case ui_manager::ZoomLevel::ZOOM_200M: zoom_name = "200m"; break;
                case ui_manager::ZoomLevel::ZOOM_100M: zoom_name = "100m"; break;
                case ui_manager::ZoomLevel::ZOOM_50M:  zoom_name = "50m"; break;
            }
            lv_label_set_text_fmt(ui.zoom_label, "[%s]", zoom_name);
            s_last_zoom = ui.current_zoom;
        }
    }

    // Get color scheme (used for GPS labels and canvas background)
    const ColorScheme& colors = getColorScheme();

    // Update GPS status label — only when fix status, sat count, or daylight mode changes
    if (ui.gps_status_label && lv_obj_is_valid(ui.gps_status_label)) {
        static bool  s_last_gps_valid   = false;
        static int   s_last_sats        = -1;
        static bool  s_last_day_status  = false;
        bool gps_valid = dev.last_gps_data.valid;
        int  sats      = dev.last_gps_data.sats;
        if (gps_valid != s_last_gps_valid || sats != s_last_sats || g_daylight_mode != s_last_day_status) {
            if (gps_valid) {
                lv_label_set_text_fmt(ui.gps_status_label, "GPS: Fixed (%d sats)", sats);
            } else {
                lv_label_set_text(ui.gps_status_label, "GPS: Searching...");
            }
            lv_obj_set_style_text_color(ui.gps_status_label,
                g_daylight_mode ? lv_color_black() : lv_color_white(), 0);
            s_last_gps_valid  = gps_valid;
            s_last_sats       = sats;
            s_last_day_status = g_daylight_mode;
        }
    }

    // Update GPS quality label — speed EMA always updated, label only when displayed value changes
    if (ui.gps_quality_label && lv_obj_is_valid(ui.gps_quality_label)) {
        if (dev.last_gps_data.valid) {
            float hdop = dev.last_gps_data.hdop;

            // Speed EMA filter: α=0.2 smooths spikes, 0.5 km/h floor eliminates stationary noise
            static float s_speed_ema = 0.0f;
            float raw_speed_kmh = (isnan(dev.last_gps_data.speed) ? 0.0f : dev.last_gps_data.speed) * 1.852f;
            s_speed_ema = 0.2f * raw_speed_kmh + 0.8f * s_speed_ema;
            float speed_kmh = (s_speed_ema < 0.5f) ? 0.0f : s_speed_ema;

            int hdop_int  = (int)hdop;
            int hdop_dec  = (int)((hdop - hdop_int) * 10) % 10;
            int speed_int = (int)(speed_kmh + 0.5f);
            bool hdop_nan = isnan(hdop);

            static int   s_last_hdop_int  = -1;
            static int   s_last_hdop_dec  = -1;
            static int   s_last_speed_int = -1;
            static bool  s_last_hdop_nan  = false;
            static bool  s_last_day_qual  = false;

            if (hdop_int != s_last_hdop_int || hdop_dec != s_last_hdop_dec ||
                speed_int != s_last_speed_int || hdop_nan != s_last_hdop_nan ||
                g_daylight_mode != s_last_day_qual) {

                if (!hdop_nan) {
                    lv_label_set_text_fmt(ui.gps_quality_label, "HDOP:%d.%d  %d km/h",
                                          hdop_int, hdop_dec, speed_int);
                } else {
                    lv_label_set_text_fmt(ui.gps_quality_label, "HDOP:---  %d km/h", speed_int);
                }
                lv_obj_set_style_text_color(ui.gps_quality_label,
                    g_daylight_mode ? lv_color_black() : lv_color_white(), 0);
                s_last_hdop_int  = hdop_int;
                s_last_hdop_dec  = hdop_dec;
                s_last_speed_int = speed_int;
                s_last_hdop_nan  = hdop_nan;
                s_last_day_qual  = g_daylight_mode;
            }
        } else {
            lv_label_set_text(ui.gps_quality_label, "");  // Hide when no fix
        }
    }

    // Compass trust indicator (Level 1 health metrics, WP-4,
    // docs/compass_calibration_foundation.md §5). Two independent signals: a live magnitude check
    // against the stored H0 baseline (compass_qmc5883l::classifyHealth — detects tilt/interference),
    // and the calibration's own quality score captured once at save time (residual/axis ratio,
    // §5.3) — the live check can't tell a stale calibration from a tilt, but a poor score at save
    // time is its own standing warning regardless of what the live reading says.
    if (ui.compass_health_label && lv_obj_is_valid(ui.compass_health_label)) {
        const settings_manager::RadarSettings& settings = settings_manager::getSettings();
        bool has_baseline = settings.compass_calibrated && settings.compass_cal_h0 > 0.0f;
        bool quality_poor = has_baseline &&
            (settings.compass_cal_residual_pct > 5.0f ||
             settings.compass_cal_axis_ratio > 1.20f ||
             settings.compass_cal_axis_ratio < 0.83f);

        const char* text = nullptr;
        uint32_t color = 0x888888;
        if (has_baseline) {
            auto health = compass_qmc5883l::classifyHealth(dev.last_compass_data, settings.compass_cal_h0);
            if (health == compass_qmc5883l::CompassHealth::DISTURBANCE) {
                text = "Compass: interference"; color = 0xFF4444;
            } else if (health == compass_qmc5883l::CompassHealth::TILTED) {
                text = "Compass: hold flat"; color = 0xFFAA00;
            } else if (quality_poor) {
                text = "Compass: recalibrate?"; color = 0xFFAA00;
            }
            // else HEALTHY with a good calibration -- text stays null, label hides.
        } else if (settings.compass_calibrated) {
            text = "Compass: recalibrate?";  // calibrated before H0 tracking existed -- no baseline
        } else {
            text = "Compass: not calibrated";
        }

        static const char* s_last_text = "";  // pointer compare -- all values above are literals
        if (text != s_last_text) {
            lv_label_set_text(ui.compass_health_label, text ? text : "");
            if (text) lv_obj_set_style_text_color(ui.compass_health_label, lv_color_hex(color), 0);
            s_last_text = text;
        }
    }

    // Background is a style on the radar object, painted by LVGL's own class draw
    // handler before radarDrawEventCb runs. Only write it when the theme actually
    // changes: lv_obj_set_style_bg_color invalidates the object, so doing it every
    // frame would queue a redundant full-screen invalidate on top of the one below.
    {
        static uint32_t s_last_bg = 0xFFFFFFFFu;
        if (colors.background != s_last_bg) {
            lv_obj_set_style_bg_color(ui.radar_obj, lv_color_hex(colors.background), 0);
            s_last_bg = colors.background;
        }
    }

    // Update center reference to current GPS position (if valid)
    // This makes the user always at the center, and waypoints move relative to user
    if (dev.last_gps_data.valid) {
        ui.center_lat = dev.last_gps_data.lat;
        ui.center_lon = dev.last_gps_data.lon;
    }

    // Heading (ui.current_heading) is driven entirely by compass via COMPASS_UPDATE queue.
    // The I2C Task reads the QMC5883L every 100ms and queues the heading to the UI Task.
    // No GPS involvement — compass controls orientation always, moving or stationary.

    // Update waypoint fix proximity sonar (drives buzzer based on GPS distance).
    // Not drawing — this stays on the update path so it keeps running at the render
    // request rate rather than at LVGL's redraw rate.
    updateWaypointFixSonar();

    // ── Fixed waypoint distance label (LVGL overlay widget, NOT canvas text) ──────
    // Updated at ~1Hz but only calls lv_label_set_text when the integer value changes.
    // Hides/shows with HUD via hideHUD()/showHUD(); managed here when waypoint changes.
    if (ui.waypoint_distance_label) {
        static int s_last_dist_display = -1;  // Cache: prevents lv_label_set_text on every frame

        if (ui.fixed_waypoint_index >= 0 && ui.fixed_waypoint_index < ui.waypoint_count
            && ui.center_lat != 0.0 && ui.center_lon != 0.0) {
            const ui_manager::Waypoint& wp = ui.waypoints[ui.fixed_waypoint_index];
            if (wp.valid) {
                double dLat = (wp.lat - ui.center_lat) * M_PI_LOCAL / 180.0;
                double dLon = (wp.lon - ui.center_lon) * M_PI_LOCAL / 180.0;
                double a = sin(dLat/2.0)*sin(dLat/2.0) +
                           cos(ui.center_lat*M_PI_LOCAL/180.0)*cos(wp.lat*M_PI_LOCAL/180.0)*
                           sin(dLon/2.0)*sin(dLon/2.0);
                float dist_m = (float)(EARTH_RADIUS_M * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));

                // Auto-unfix when waypoint goes beyond 1km — outside navigation reach.
                // All waypoints become visible again on the next draw cycle.
                if (dist_m > 1000.0f) {
                    Serial.printf("[NAV] Fixed waypoint auto-released: %.0fm > 1km\n", dist_m);
                    ui.fixed_waypoint_index = -1;
                    s_last_dist_display = -1;
                    lv_obj_add_flag(ui.waypoint_distance_label, LV_OBJ_FLAG_HIDDEN);
                    buzzer::stopSonar();
                } else {
                    int dist_display = (int)dist_m;
                    if (dist_display != s_last_dist_display) {
                        s_last_dist_display = dist_display;
                        char buf[16];
                        snprintf(buf, sizeof(buf), "Fixed: %dm", dist_display);
                        lv_label_set_text(ui.waypoint_distance_label, buf);
                    }
                    if (ui.hud_visible)
                        lv_obj_clear_flag(ui.waypoint_distance_label, LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            // No fixed waypoint — hide and invalidate cache so next fix triggers a fresh draw
            s_last_dist_display = -1;
            lv_obj_add_flag(ui.waypoint_distance_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Beacon-found indicator: show the circle+star overlay when beacon is marked found
    // and HUD is visible. Hides with the rest of the HUD.
    if (ui.beacon_found_canvas) {
        bool should_show = beacon_proximity::isFound() && ui.hud_visible;
        if (should_show)
            lv_obj_clear_flag(ui.beacon_found_canvas, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(ui.beacon_found_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    // Request the repaint. Everything above only touched widgets and state; the
    // geometry is painted by radarDrawEventCb when LVGL services this invalidate.
    lv_obj_invalidate(ui.radar_obj);

    // ---- DEV perf HUD -------------------------------------------------
    // label = this function (widget updates + invalidate). refr = LVGL's refresh,
    // captured by lvgl_monitor_cb, which now CONTAINS the radar painting since it
    // happens in a draw event. So the frame is label + refr — adding paint would
    // count the geometry twice.
    g_nav_state.label_us = (uint32_t)(esp_timer_get_time() - t_label_start);

    if (ui.perf_label && lv_obj_is_valid(ui.perf_label) &&
        !lv_obj_has_flag(ui.perf_label, LV_OBJ_FLAG_HIDDEN)) {

        // FPS measured from actual panel flushes over a 1 s window
        static uint32_t s_last_fps_time  = 0;
        static uint32_t s_last_flush_cnt = 0;
        static float    s_fps            = 0.0f;
        uint32_t now = millis();
        if (now - s_last_fps_time >= 1000) {
            uint32_t flushes = g_nav_state.flush_count - s_last_flush_cnt;
            s_fps = (flushes * 1000.0f) / (float)(now - s_last_fps_time);
            s_last_flush_cnt = g_nav_state.flush_count;
            s_last_fps_time  = now;
        }

        uint32_t paint_ms_x10 = g_nav_state.paint_us / 100;   // 0.1 ms resolution
        uint32_t refr_ms      = g_nav_state.refr_ms;
        uint32_t label_ms_x10 = g_nav_state.label_us / 100;
        // Frame = label + refr. paint is shown for attribution but is INSIDE refr.
        uint32_t total_ms_x10 = label_ms_x10 + refr_ms * 10;

        // I2C health, added 2026-08-02 for the FT-06 freeze investigation — this is
        // the only live telemetry channel in the field, since a freeze happens on
        // battery where serial isn't attached. Cheap: reads counters, no I2C traffic.
        const auto& i2c_stats = i2c_manager::getStats();

        lv_label_set_text_fmt(ui.perf_label,
                              "grid %u wpt %u rot %u ms\n"
                              "paint %u.%ums (in refr %ums)\n"
                              "total %u.%ums  %d.%d fps\n"
                              "i2c fail %lu/%lu consec %lu",
                              (unsigned)(g_nav_state.grid_us / 1000),
                              (unsigned)(g_nav_state.wpt_us / 1000),
                              (unsigned)(g_nav_state.rot_us / 1000),
                              (unsigned)(paint_ms_x10 / 10), (unsigned)(paint_ms_x10 % 10),
                              (unsigned)refr_ms,
                              (unsigned)(total_ms_x10 / 10),
                              (unsigned)(total_ms_x10 % 10),
                              (int)s_fps, (int)((s_fps - (int)s_fps) * 10),
                              (unsigned long)i2c_stats.failed_ops,
                              (unsigned long)i2c_stats.total_ops,
                              (unsigned long)i2c_stats.consecutive_failures);
    }
}

void handleTapAt(int screen_x, int screen_y) {
    // Called from the LVGL LV_EVENT_PRESSED callback on the radar stage.
    // Fires at LVGL input-poll rate (~10ms), so even quick taps are caught.
    // Only active when the radar screen is loaded and waypoints are present.
    ui_manager::UIState& ui = ui_manager::getUIState();

    // Beacon found: tap the orange ball (within 60px of center) when CLOSE zone
    if (ui.current_zoom == ui_manager::ZoomLevel::ZOOM_50M
        && !beacon_proximity::isFound()
        && beacon_proximity::getCurrentZone() == beacon_proximity::ProximityZone::CLOSE) {
        const int cx = system_config::display::SCREEN_WIDTH / 2;
        const int cy = system_config::display::SCREEN_HEIGHT / 2;
        int dx = screen_x - cx;
        int dy = screen_y - cy;
        if (dx*dx + dy*dy <= 60*60) {
            beacon_proximity::setFound(true);
            buzzer::chirp(80);  // Confirmation chirp
            return;
        }
    }

    if (ui.waypoint_count == 0) return;

    const int screen_size = system_config::display::SCREEN_WIDTH;
    constexpr int HIT_RADIUS = 28;  // Slightly larger than the 25px dot for usability

    for (int i = 0; i < ui.waypoint_count; i++) {
        if (!ui.waypoints[i].valid) continue;
        int wx, wy;
        latLonToScreen(ui.waypoints[i].lat, ui.waypoints[i].lon, wx, wy, screen_size);
        if (wx < 0 || wx >= screen_size || wy < 0 || wy >= screen_size) continue;
        int dx = screen_x - wx;
        int dy = screen_y - wy;
        if (dx*dx + dy*dy <= HIT_RADIUS*HIT_RADIUS) {
            Serial.printf("[NAVIGATION] Waypoint %d tapped: %s\n", i,
                          ui.waypoints[i].display_name[0] ? ui.waypoints[i].display_name
                                                           : ui.waypoints[i].name);
            // ui.waypoints[]/selected_waypoint_index are no longer UI-Task-exclusive:
            // the System Task's movement-triggered reselect (see task_manager.cpp,
            // gpx_loader::reselect()) can recycle slots concurrently once the PSRAM
            // index outgrows the working set. Mutex-protect these writes to match.
            bool mx = (task_manager::ui_state_mutex != nullptr) &&
                      (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
            ui.selected_waypoint_index = i;
            if (mx) xSemaphoreGive(task_manager::ui_state_mutex);

            // Mark as found when tapping the fixed waypoint while within GPS close range.
            // 15m threshold accounts for typical GPS accuracy (±5-10m).
            if (i == ui.fixed_waypoint_index && !ui.waypoints[i].found
                && ui.center_lat != 0.0 && ui.center_lon != 0.0) {
                double dLat = (ui.waypoints[i].lat - ui.center_lat) * M_PI_LOCAL / 180.0;
                double dLon = (ui.waypoints[i].lon - ui.center_lon) * M_PI_LOCAL / 180.0;
                double fa = sin(dLat/2.0)*sin(dLat/2.0) +
                            cos(ui.center_lat*M_PI_LOCAL/180.0)*cos(ui.waypoints[i].lat*M_PI_LOCAL/180.0)*
                            sin(dLon/2.0)*sin(dLon/2.0);
                float found_dist = (float)(EARTH_RADIUS_M * 2.0 * atan2(sqrt(fa), sqrt(1.0 - fa)));
                if (found_dist <= 15.0f) {
                    bool mxf = (task_manager::ui_state_mutex != nullptr) &&
                               (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
                    ui.waypoints[i].found = true;
                    if (mxf) xSemaphoreGive(task_manager::ui_state_mutex);
                    // Write-through into the PSRAM index so "found" survives this
                    // slot being recycled by a future reselect (see ADR-0023).
                    gpx_loader::markWaypointFound(i, true);
                    buzzer::chirp(80);
                    Serial.printf("[NAVIGATION] Waypoint %d FOUND at %.1fm\n", i, found_dist);
                }
            }

            goToWaypointScreen();
            return;
        }
    }
}

} // namespace navigation