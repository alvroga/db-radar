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
#include "core/arduino_compat.h"
#include <cfloat>
#include "esp_heap_caps.h"
#include "esp_system.h"

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

    // Refresh canvas
    if (ui.radar_canvas && lv_obj_is_valid(ui.radar_canvas)) {
        Serial.println("[NAVIGATION] Radar canvas valid - updating display");
        updateRadarDisplay();
    } else {
        Serial.println("[NAVIGATION] WARNING: Radar canvas invalid - skipping display update");
    }
}

void goToSettingsScreen() {
    ui_manager::UIState& ui = ui_manager::getUIState();

    // Always recreate to pick up fresh runtime state (Found/Missing, WiFi status, etc.)
    if (ui.screen_settings && lv_obj_is_valid(ui.screen_settings)) {
        lv_obj_del(ui.screen_settings);
        ui.screen_settings      = nullptr;
        ui.settings_tabview     = nullptr;
        ui.settings_tab_gps     = nullptr;
        ui.settings_tab_wifi    = nullptr;
        ui.settings_tab_display = nullptr;
        ui.settings_tab_sound   = nullptr;
        ui.settings_tab_beacon  = nullptr;
        ui.settings_tab_dev     = nullptr;
    }
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

    // Convert degrees to radians
    double lat1 = ui.center_lat * M_PI_LOCAL / 180.0;
    double lat2 = lat * M_PI_LOCAL / 180.0;
    double dLat = (lat - ui.center_lat) * M_PI_LOCAL / 180.0;
    double dLon = (lon - ui.center_lon) * M_PI_LOCAL / 180.0;

    // Calculate distance in meters using Haversine formula
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1) * cos(lat2) *
               sin(dLon / 2.0) * sin(dLon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    double distance = EARTH_RADIUS_M * c;

    // Calculate bearing (direction from center to point)
    double y_component = sin(dLon) * cos(lat2);
    double x_component = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
    double bearing = atan2(y_component, x_component);

    // Convert to screen coordinates
    // North is up (negative Y), East is right (positive X)
    double dx_meters = distance * sin(bearing);
    double dy_meters = -distance * cos(bearing);  // Negative because screen Y increases downward

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

void drawRadarGrid(lv_obj_t* canvas, int screen_size, int grid_spacing_pixels) {
    if (!canvas) return;

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
        lv_canvas_draw_line(canvas, points, 2, &line_dsc);
    }

    // Always draw right edge line
    if ((screen_size - 1) % grid_spacing_pixels != 0) {
        lv_point_t points[2];
        points[0].x = screen_size - 1;
        points[0].y = 0;
        points[1].x = screen_size - 1;
        points[1].y = screen_size - 1;
        lv_canvas_draw_line(canvas, points, 2, &line_dsc);
    }

    // Draw horizontal lines from y=0 to y=screen_size-1
    for (int y = 0; y < screen_size; y += grid_spacing_pixels) {
        lv_point_t points[2];
        points[0].x = 0;
        points[0].y = y;
        points[1].x = screen_size - 1;
        points[1].y = y;
        lv_canvas_draw_line(canvas, points, 2, &line_dsc);
    }

    // Always draw bottom edge line
    if ((screen_size - 1) % grid_spacing_pixels != 0) {
        lv_point_t points[2];
        points[0].x = 0;
        points[0].y = screen_size - 1;
        points[1].x = screen_size - 1;
        points[1].y = screen_size - 1;
        lv_canvas_draw_line(canvas, points, 2, &line_dsc);
    }
}

void drawCenterTriangle(lv_obj_t* canvas, int screen_size) {
    if (!canvas) return;

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

    lv_canvas_draw_polygon(canvas, points, 3, &tri_dsc);
}

void drawNorthIndicator(lv_obj_t* canvas, int screen_size) {
    if (!canvas) return;

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
    lv_canvas_draw_arc(canvas, north_x, north_y, 15, 0, 360, &circle_dsc);

    // Draw "N" text (white in normal mode, white/yellow in daylight for contrast)
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_white();  // White "N" visible on red/dark red background
    label_dsc.font = &iosevka_16;

    // Center the "N" text (approximate offset for single character)
    lv_canvas_draw_text(canvas, north_x - 5, north_y - 7, 20, &label_dsc, "N");
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
static void drawBeaconProximityGauge(lv_obj_t* canvas, int screen_size) {
    if (!canvas) return;

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
            lv_canvas_draw_rect(canvas, 0, 0, screen_size, screen_size, &fill_dsc);

            // Orange ball at center
            const int ball_r = 60;
            lv_draw_rect_dsc_t ball_dsc;
            lv_draw_rect_dsc_init(&ball_dsc);
            ball_dsc.bg_color     = lv_color_hex(0xFF6B00);
            ball_dsc.bg_opa       = LV_OPA_COVER;
            ball_dsc.radius       = LV_RADIUS_CIRCLE;
            ball_dsc.border_width = 0;
            lv_canvas_draw_rect(canvas, cx - ball_r, cy - ball_r,
                                ball_r * 2, ball_r * 2, &ball_dsc);

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
                lv_canvas_draw_polygon(canvas, arm, 3, &star_dsc);
            }
            // Center pentagon (convex)
            lv_canvas_draw_polygon(canvas, s_inner, 5, &star_dsc);
            return;
        }

        // VERY_FAR / FAR / MEDIUM: ring flush with screen edge, grows inward by zone
        // radius = 239 - width/2 keeps outer edge at 239px regardless of ring thickness
        int32_t ring_width = 0;
        switch (state.zone) {
            case beacon_proximity::ProximityZone::VERY_FAR: ring_width = 8;  break;
            case beacon_proximity::ProximityZone::FAR:      ring_width = 16; break;
            case beacon_proximity::ProximityZone::MEDIUM:   ring_width = 28; break;
            default: return;  // OUT_OF_RANGE — no ring
        }

        lv_draw_arc_dsc_t arc_dsc;
        lv_draw_arc_dsc_init(&arc_dsc);
        arc_dsc.color = g_daylight_mode ? lv_color_hex(0x1565C0) : lv_color_hex(0x4488FF);
        arc_dsc.width = ring_width;
        arc_dsc.opa   = LV_OPA_COVER;
        int32_t ring_radius = 239 - ring_width / 2;  // outer edge always at 239px
        lv_canvas_draw_arc(canvas, cx, cy, ring_radius, 0, 360, &arc_dsc);
    }
}

void drawOffScreenIndicator(lv_obj_t* canvas, double bearing, int screen_size) {
    if (!canvas) return;

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
    lv_canvas_draw_polygon(canvas, points, 3, &border_dsc);

    // Draw triangle fill (color depends on mode)
    lv_draw_rect_dsc_t tri_dsc;
    lv_draw_rect_dsc_init(&tri_dsc);
    tri_dsc.bg_color = lv_color_hex(colors.offscreen_indicator);
    tri_dsc.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_polygon(canvas, points, 3, &tri_dsc);
}

void drawWaypoints(lv_obj_t* canvas, int screen_size) {
    if (!canvas) return;

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

    // Pre-compute user's lat in radians + trig values — constant for all waypoints this frame
    double lat1      = ui.center_lat * M_PI_LOCAL / 180.0;
    double cos_lat1  = cos(lat1);
    double sin_lat1  = sin(lat1);

    // Track fixed waypoint if it ends up off-screen (drawn separately, bypasses clustering)
    bool fixed_off_screen = false;
    double fixed_off_bearing = 0.0;

    // Process all waypoints
    for (int i = 0; i < ui.waypoint_count; i++) {
        if (!ui.waypoints[i].valid) continue;
        // When a waypoint is fixed, render only that target — everything else is noise.
        // Eliminates N Haversine calcs, N polygon draws, and all sector clustering.
        if (ui.fixed_waypoint_index >= 0 && i != ui.fixed_waypoint_index) continue;

        // Convert lat/lon to screen coordinates using current zoom
        int x, y;
        double lat2     = ui.waypoints[i].lat * M_PI_LOCAL / 180.0;
        double cos_lat2 = cos(lat2);
        double sin_lat2 = sin(lat2);
        double dLat = (ui.waypoints[i].lat - ui.center_lat) * M_PI_LOCAL / 180.0;
        double dLon = (ui.waypoints[i].lon - ui.center_lon) * M_PI_LOCAL / 180.0;

        // Haversine formula for distance (reuses pre-computed cos_lat1)
        double sinHalfDLat = sin(dLat / 2.0);
        double sinHalfDLon = sin(dLon / 2.0);
        double a = sinHalfDLat * sinHalfDLat +
                   cos_lat1 * cos_lat2 * sinHalfDLon * sinHalfDLon;
        double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
        double distance = EARTH_RADIUS_M * c;

        // STRATEGY 1: Distance filtering - skip waypoints beyond maximum relevant distance
        // This eliminates waypoints thousands of km away
        if (distance > max_indicator_distance) {
            continue;  // Too far away, not relevant to current navigation
        }

        // Calculate bearing — reuses cos_lat1, sin_lat1, cos_lat2, sin_lat2
        double y_component = sin(dLon) * cos_lat2;
        double x_component = cos_lat1 * sin_lat2 - sin_lat1 * cos_lat2 * cos(dLon);
        double bearing = atan2(y_component, x_component);

        // Convert to screen coordinates
        double dx_meters = distance * sin(bearing);
        double dy_meters = -distance * cos(bearing);

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
            lv_canvas_draw_rect(canvas, x - half_size, y - half_size, size, size, &circle_dsc);

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
                    lv_canvas_draw_polygon(canvas, arm, 3, &star_dsc);
                }
                lv_canvas_draw_polygon(canvas, sp_inner, 5, &star_dsc);
            }
        } else {
            if (i == ui.fixed_waypoint_index) {
                // Fixed waypoint off-screen: record bearing, draw separately with pulse
                fixed_off_screen = true;
                fixed_off_bearing = bearing;
            } else {
                // Off-screen: STRATEGY 2 - Sector clustering
                // Bearing: -π to π, convert to 0-360 degrees
                float bearing_deg = bearing * 180.0f / M_PI_LOCAL;
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
            drawOffScreenIndicator(canvas, bearing, screen_size);
        }
    }

    // Draw fixed waypoint off-screen arrow separately — bypasses clustering
    if (fixed_off_screen) {
        double bearing = fixed_off_bearing;
        if (ui.heading_up_mode && ui.current_heading != 0.0f) {
            bearing -= ui.current_heading * M_PI_LOCAL / 180.0;
        }
        drawOffScreenIndicator(canvas, bearing, screen_size);
    }
}

/**
 * @brief Drive proximity sonar for a fixed (user-selected) waypoint.
 *
 * Maps GPS distance to sonar interval, taking sonar priority from beacon proximity.
 * Called every time updateRadarDisplay() runs (~5Hz with GPS lock).
 * Respects the master sound setting (button_sound_enabled).
 */
static void updateWaypointFixSonar() {
    ui_manager::UIState& ui = ui_manager::getUIState();

    if (ui.fixed_waypoint_index < 0 || ui.fixed_waypoint_index >= ui.waypoint_count) {
        beacon_proximity::suppressSonar(false);
        return;
    }

    // Proximity sonar only active at 50m zoom (same as beacon proximity)
    if (ui.current_zoom != ui_manager::ZoomLevel::ZOOM_50M) {
        beacon_proximity::suppressSonar(false);
        buzzer::stopSonar();
        return;
    }

    const auto& settings = settings_manager::getSettings();
    if (!settings.button_sound_enabled) {
        beacon_proximity::suppressSonar(false);
        return;
    }

    const ui_manager::Waypoint& wp = ui.waypoints[ui.fixed_waypoint_index];
    if (!wp.valid || ui.center_lat == 0.0 || ui.center_lon == 0.0) {
        beacon_proximity::suppressSonar(false);
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

    // Map distance to sonar interval — 4 zones, silent beyond 50m
    uint32_t interval_ms = 0;
    if      (distance_m <=  5.0f)  interval_ms = 250;
    else if (distance_m <= 10.0f)  interval_ms = 500;
    else if (distance_m <= 30.0f)  interval_ms = 750;
    else if (distance_m <= 50.0f)  interval_ms = 1500;

    // Suppress beacon sonar so waypoint fix has priority
    beacon_proximity::suppressSonar(true);

    if (interval_ms > 0) buzzer::setSonarInterval(interval_ms, 30);
    else                  buzzer::stopSonar();
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

void updateRadarDisplay() {
    ui_manager::UIState& ui = ui_manager::getUIState();
    const device_manager::DeviceState& dev = device_manager::getDeviceState();

    // Comprehensive object validation before any operations
    if (!ui.radar_canvas) {
        Serial.println("[RADAR] ERROR: radar_canvas is NULL!");
        return;
    }

    if (!lv_obj_is_valid(ui.radar_canvas)) {
        Serial.println("[RADAR] ERROR: radar_canvas is invalid (possibly deleted)!");
        return;
    }

    int screen_size = system_config::display::SCREEN_WIDTH;

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

    // Clear canvas with appropriate background color (colors already fetched above)
    lv_canvas_fill_bg(ui.radar_canvas, lv_color_hex(colors.background), LV_OPA_COVER);

    // Get grid spacing from current zoom level
    int grid_spacing_pixels = ui.getGridSpacingPixels(screen_size);

    // Draw grid (black lines) - perfectly aligned at all zoom levels
    drawRadarGrid(ui.radar_canvas, screen_size, grid_spacing_pixels);

    // Update center reference to current GPS position (if valid)
    // This makes the user always at the center, and waypoints move relative to user
    if (dev.last_gps_data.valid) {
        ui.center_lat = dev.last_gps_data.lat;
        ui.center_lon = dev.last_gps_data.lon;
    }

    // Heading (ui.current_heading) is driven entirely by compass via COMPASS_UPDATE queue.
    // The I2C Task reads the QMC5883L every 100ms and queues the heading to the UI Task.
    // No GPS involvement — compass controls orientation always, moving or stationary.

    // Draw center triangle (red equilateral - always at center representing user)
    drawCenterTriangle(ui.radar_canvas, screen_size);

    // Update waypoint fix proximity sonar (drives buzzer based on GPS distance)
    updateWaypointFixSonar();

    // Draw waypoints (yellow circles) - they move as user moves
    // Drawn after triangle so they appear on top
    drawWaypoints(ui.radar_canvas, screen_size);

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

    // Draw north indicator (shows where north is in heading-up mode)
    drawNorthIndicator(ui.radar_canvas, screen_size);

    // Draw beacon proximity gauge (arc grows from top as signal strengthens)
    // Drawn last so it overlays everything including the north indicator
    drawBeaconProximityGauge(ui.radar_canvas, screen_size);

    // Beacon-found indicator: show the circle+star overlay when beacon is marked found
    // and HUD is visible. Hides with the rest of the HUD.
    if (ui.beacon_found_canvas) {
        bool should_show = beacon_proximity::isFound() && ui.hud_visible;
        if (should_show)
            lv_obj_clear_flag(ui.beacon_found_canvas, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(ui.beacon_found_canvas, LV_OBJ_FLAG_HIDDEN);
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
            ui.selected_waypoint_index = i;

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
                    ui.waypoints[i].found = true;
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