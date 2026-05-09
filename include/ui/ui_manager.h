#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <lvgl.h>
#include "system_config.h"
#include "fonts/custom_fonts.h"

namespace ui_manager {

// UI Configuration - uses central system configuration with override capability
struct Config {
    int screen_width = system_config::display::SCREEN_WIDTH;
    int screen_height = system_config::display::SCREEN_HEIGHT;
    int header_height = system_config::ui::HEADER_HEIGHT;
    int circle_radius = system_config::display::SCREEN_WIDTH / 2;  // Half screen width
    int safe_margin = system_config::ui::SAFE_MARGIN;
    const lv_font_t* font = &iosevka_16;
};

// Waypoint storage
struct Waypoint {
    double lat = 0.0;
    double lon = 0.0;
    bool valid = false;
    bool found = false;         // Marked found by tapping while within 15m
    char name[48] = {};         // Cache code (e.g. GC38EVJ) or waypoint name tag
    char display_name[64] = {}; // Human-readable title (groundspeak:name or fallback to name)
    char desc[1024] = {};       // Description (short+long combined, HTML stripped, capped)
    char hint[256] = {};        // Hint (groundspeak:encoded_hints, empty for plain waypoints)
};

// Zoom levels for radar display
enum class ZoomLevel {
    ZOOM_1KM   = 0,  // 1km radius - overview
    ZOOM_500M  = 1,  // 500m radius - neighborhood
    ZOOM_200M  = 2,  // 200m radius - local area
    ZOOM_100M  = 3,  // 100m radius - street level
    ZOOM_50M   = 4   // 50m radius - precision mode
};

// Zoom configuration for each level
struct ZoomConfig {
    float radius_meters;        // Radius visible from center (e.g., 10000 for 10km)
    float grid_spacing_meters;  // Distance between grid lines

    // Calculate meters per pixel based on screen size
    float getMetersPerPixel(int screen_size) const {
        // radius_meters should fit in half the screen (from center to edge)
        return (2.0f * radius_meters) / screen_size;
    }

    // Get grid spacing in pixels
    int getGridSpacingPixels(int screen_size) const {
        float mpp = getMetersPerPixel(screen_size);
        return (int)(grid_spacing_meters / mpp);
    }
};

// Radar configuration
struct RadarConfig {
    static constexpr int MAX_WAYPOINTS = 50;
    static constexpr int CENTER_TRIANGLE_SIDE = 44;          // Center triangle: 44px sides
    static constexpr int CENTER_TRIANGLE_HEIGHT = 38;        // Center triangle: 38px height
    static constexpr int POSITION_TRIANGLE_SIZE = 15;        // GPS position triangle size
    static constexpr int WAYPOINT_SIZE = 25;                 // Yellow beacon: 25x25 circle

    // Off-screen indicator configuration
    static constexpr int MAX_OFFSCREEN_INDICATORS = 8;       // Max indicators (one per sector)
    static constexpr float DISTANCE_FILTER_MULTIPLIER = 10.0f;  // Show waypoints within 10× zoom radius
    static constexpr int INDICATOR_SECTORS = 8;              // 8 sectors: N, NE, E, SE, S, SW, W, NW
    static constexpr int INDICATOR_SIZE = 25;                // Triangle size (pixels) - increased for visibility
    static constexpr int INDICATOR_EDGE_INSET = 25;          // Inset from circular edge (pixels)
    static constexpr int INDICATOR_BORDER_WIDTH = 3;         // Border width (pixels) for better visibility

    // Waypoint glow effect configuration (analog radar aesthetic)
    static constexpr int WAYPOINT_GLOW_RADIUS = 18;          // Shadow width in pixels
    static constexpr uint32_t WAYPOINT_GLOW_COLOR = 0xFFFF88;  // Soft yellow-white glow
    static constexpr lv_opa_t WAYPOINT_GLOW_OPACITY = LV_OPA_40;  // 16% opacity (40/255)
    static constexpr int WAYPOINT_GLOW_SPREAD = 2;           // Shadow spread in pixels

    // Zoom level configurations: 50→100→200→500→1000m (progressive grid density)
    // Grid cell count increases as you zoom out: 2×2 → 4×4 → 6×6 → 8×8 → 10×10
    // Pixel spacing: 1km=48px, 500m=60px, 200m=80px, 100m=120px, 50m=240px
    static constexpr ZoomConfig ZOOM_CONFIGS[] = {
        {1000.0f,  200.0f},    // 1km radius, 200m grid  = 48px spacing  (10×10 grid)
        {500.0f,   125.0f},    // 500m radius, 125m grid  = 60px spacing  (8×8  grid)
        {200.0f,   66.67f},    // 200m radius, 66.7m grid = 80px spacing  (6×6  grid)
        {100.0f,   50.0f},     // 100m radius, 50m  grid  = 120px spacing (4×4  grid)
        {50.0f,    50.0f}      // 50m  radius, 50m  grid  = 240px spacing (2×2  grid)
    };

    static constexpr int NUM_ZOOM_LEVELS = 5;
    static constexpr ZoomLevel DEFAULT_ZOOM = ZoomLevel::ZOOM_100M;  // Start at street level
};

// UI State - holds references to all UI objects
struct UIState {
    // Screens
    lv_obj_t* screen_main = nullptr;
    lv_obj_t* screen_radar = nullptr;
    lv_obj_t* screen_settings = nullptr;
    lv_obj_t* screen_loading = nullptr;  // Loading screen with spinner
    lv_obj_t* screen_ap = nullptr;       // AP upload mode screen (only in AP boot)
    lv_obj_t* screen_wifi = nullptr;     // WiFi STA mode screen (only in WiFi boot)
    lv_obj_t* loading_status_label = nullptr;  // Loading status text (for boot messages)

    // Header controls (shared header on top layer)
    lv_obj_t* header = nullptr;
    lv_obj_t* header_label = nullptr;
    lv_obj_t* btn_back = nullptr;

    // Radar elements
    lv_obj_t* radar_canvas = nullptr;
    lv_obj_t* btn_add_waypoint = nullptr;
    lv_obj_t* zoom_label = nullptr;        // Zoom level indicator (always visible)
    lv_obj_t* gps_status_label = nullptr;  // GPS status indicator (sats)
    lv_obj_t* gps_quality_label = nullptr; // GPS quality indicator (HDOP, speed)
    lv_obj_t* battery_label = nullptr;     // Battery status indicator
    lv_obj_t* log_indicator = nullptr;     // Log enabled indicator (DEV mode)
    lv_obj_t* beacon_dbm_label = nullptr;  // Beacon RSSI overlay (DEV mode, 50m zoom only)
    lv_obj_t* center_label = nullptr;      // Center text label (test for custom font)
    lv_obj_t* wifi_mode_label = nullptr;   // WiFi mode overlay (shown when radar is disabled)
    lv_obj_t* waypoint_distance_label = nullptr;  // Distance to fixed waypoint (LVGL overlay, not canvas draw)
    lv_obj_t* beacon_found_canvas = nullptr;       // Beacon-found indicator (circle + star, shown when isFound())

    // Settings elements
    lv_obj_t* settings_tabview = nullptr;
    lv_obj_t* settings_tab_gps = nullptr;
    lv_obj_t* settings_tab_wifi = nullptr;
    lv_obj_t* settings_tab_display = nullptr;
    lv_obj_t* settings_tab_sound = nullptr;
    lv_obj_t* settings_tab_beacon = nullptr;
    lv_obj_t* settings_tab_dev = nullptr;

    // Waypoint storage
    Waypoint waypoints[RadarConfig::MAX_WAYPOINTS];
    int waypoint_count = 0;
    int selected_waypoint_index = -1;  // Index of tapped waypoint (-1 = none)
    int fixed_waypoint_index = -1;     // Index of "fixed" waypoint for proximity sonar (-1 = none)
    lv_obj_t* screen_waypoint = nullptr;  // Waypoint detail screen (created on demand)

    // Radar state
    double center_lat = 0.0;        // Reference center point (user's current position)
    double center_lon = 0.0;
    ZoomLevel current_zoom = RadarConfig::DEFAULT_ZOOM;  // Current zoom level

    // Heading-up navigation mode
    bool heading_up_mode = true;         // Default: heading-up (user request)
    float current_heading = 0.0;         // Current GPS heading (0-360°, 0=North)
    float last_valid_heading = 0.0;      // Last heading when moving (fallback)
    uint32_t last_heading_update = 0;    // Timestamp of last heading update

    // HUD visibility management
    bool hud_visible = true;             // HUD elements currently visible
    uint32_t last_interaction_ms = 0;    // Last screen touch/interaction time
    bool hud_auto_hide_enabled = true;   // Cached: auto-hide enabled
    uint32_t hud_auto_hide_delay_ms = 10000;  // Cached: auto-hide delay in milliseconds

    // Get current meters per pixel based on zoom level
    float getMetersPerPixel(int screen_size) const {
        int zoom_idx = static_cast<int>(current_zoom);
        return RadarConfig::ZOOM_CONFIGS[zoom_idx].getMetersPerPixel(screen_size);
    }

    // Get current grid spacing in pixels
    int getGridSpacingPixels(int screen_size) const {
        int zoom_idx = static_cast<int>(current_zoom);
        return RadarConfig::ZOOM_CONFIGS[zoom_idx].getGridSpacingPixels(screen_size);
    }

    // Cycle to next zoom level
    void cycleZoom() {
        int current = static_cast<int>(current_zoom);
        current = (current + 1) % RadarConfig::NUM_ZOOM_LEVELS;
        current_zoom = static_cast<ZoomLevel>(current);
    }

    // Cycle to previous zoom level
    void cycleZoomReverse() {
        int current = static_cast<int>(current_zoom);
        current = (current - 1 + RadarConfig::NUM_ZOOM_LEVELS) % RadarConfig::NUM_ZOOM_LEVELS;
        current_zoom = static_cast<ZoomLevel>(current);
    }

    // Reset zoom to default
    void resetZoom() {
        current_zoom = RadarConfig::DEFAULT_ZOOM;
    }
};

// Initialize the UI system and create all screens
bool init(const Config& config = Config{});

// Get reference to UI state (for external access to UI objects)
UIState& getUIState();

// Screen creation functions
void createLoadingScreen();    // Loading screen with ease-in-out spinner
void createRadarScreen();      // Radar is the main/default screen
void createSettingsScreen();   // Settings screen with GPS configuration
void createAPScreen();         // AP upload mode full-screen (only called in AP boot)
void createWiFiScreen();       // WiFi STA mode full-screen (only called in WiFi boot)

// Loading screen status updates (fun boot messages)
void updateLoadingStatus(const char* message);  // Update loading screen status text

// Header management
void showBackButton(bool show);
void setHeaderTitle(const char* title);

// HUD visibility management
void updateHUDVisibility();        // Call periodically to manage auto-hide
void showHUD();                     // Show all HUD elements
void hideHUD();                     // Hide all HUD elements
void onScreenTouch();               // Call when screen is touched
void reloadHUDSettings();           // Reload HUD settings from NVS (call after settings change)

// Daylight mode support
void updateDaylightMode(bool daylight_enabled);  // Update UI elements for daylight mode

// Utility functions

// Memory management functions
void cleanupScreen(lv_obj_t* screen);
void cleanupAllScreens();
bool safeScreenLoad(lv_obj_t* screen);
void stopAllTimers();

} // namespace ui_manager

#endif // UI_MANAGER_H