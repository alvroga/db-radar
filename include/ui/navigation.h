#ifndef NAVIGATION_H
#define NAVIGATION_H

#include <lvgl.h>
#include "ui_manager.h"
#include "device_manager.h"

namespace navigation {

// Navigation state - simplified for radar project
struct NavState {
    // Touch tracking
    volatile int16_t touch_x = 240;
    volatile int16_t touch_y = 240;
    volatile bool touch_pressed = false;

    // FPS tracking
    volatile uint32_t flush_count = 0;

    // Render timing instrumentation (DEV mode HUD).
    // Splits a radar frame into its two measurable halves so the cost of
    // software rotation can be attributed:
    //   paint_us -> canvas painting (lv_canvas_draw_* calls in updateRadarDisplay)
    //   refr_ms  -> LVGL refresh: canvas blit + 90 deg rotate + flush dispatch
    volatile uint32_t paint_us = 0;   // last updateRadarDisplay() duration, microseconds
    volatile uint32_t refr_ms  = 0;   // last LVGL refresh duration, milliseconds
    volatile uint32_t refr_px  = 0;   // pixels touched by last LVGL refresh

    // Per-stage breakdown of the paint half, so the dominant cost can be
    // identified rather than inferred (all microseconds)
    volatile uint32_t bg_us    = 0;   // lv_canvas_fill_bg (full 480x480 clear)
    volatile uint32_t grid_us  = 0;   // drawRadarGrid
    volatile uint32_t wpt_us   = 0;   // drawWaypoints
    volatile uint32_t deco_us  = 0;   // triangle + north indicator + beacon gauge

    // First frame tracking
    volatile bool first_frame_done = false;
};

// Initialize navigation system
bool init();

// Get navigation state
NavState& getNavState();

// Navigation utilities
void goToMainScreen();
void goToRadarScreen();
void goToSettingsScreen();
void goToDevScreen();
void goToWaypointScreen();  // Open detail screen for selected_waypoint_index
void showSettings();  // Alias for goToSettingsScreen (used by dev_screen)

// Radar drawing functions
void updateRadarDisplay();
void drawRadarGrid(lv_obj_t* canvas, int screen_size, int grid_spacing_pixels);
void drawCenterTriangle(lv_obj_t* canvas, int screen_size);
void drawWaypoints(lv_obj_t* canvas, int screen_size);

// Draw the beacon-found indicator icon into a 32×32 TRUE_COLOR_ALPHA canvas.
// Call once on creation and again whenever the daylight theme changes.
void drawBeaconFoundIndicator(lv_obj_t* canvas, bool daylight);

// GPS coordinate conversion
void latLonToScreen(double lat, double lon, int& x, int& y, int screen_size);
float metersToPixels(float meters, float meters_per_pixel);

// Waypoint tap detection — call from LVGL touch event callback
void handleTapAt(int screen_x, int screen_y);

// Heading smoothing (used by task_manager COMPASS_UPDATE handler)
constexpr float HEADING_SMOOTHING = 0.3f;  // 0=no update, 1=instant; 0.3 = smooth at 10Hz (was 0.8 at 1Hz)
float smoothHeading(float current_heading, float target_heading, float smoothing_factor);

} // namespace navigation

#endif // NAVIGATION_H