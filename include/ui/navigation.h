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
    //
    // Since the radar draws in LV_EVENT_DRAW_MAIN, painting happens *inside* the
    // LVGL refresh rather than before it. paint_us is therefore a component of
    // refr_ms, NOT something to add to it:
    //
    //   frame = label_us + refr_ms
    //   refr_ms = paint_us (geometry) + rot_us + flush_us + LVGL overhead
    //
    // Adding paint_us to refr_ms double-counts — which is what the pre-step-8
    // FRAME TOTAL did, correctly, back when the canvas made them sequential.
    volatile uint32_t paint_us = 0;   // radar DRAW_MAIN handler — NESTED inside refr_ms
    volatile uint32_t label_us = 0;   // updateRadarDisplay() HUD label work (outside the refresh)
    volatile uint32_t refr_ms  = 0;   // last LVGL refresh duration, milliseconds
    volatile uint32_t rot_us   = 0;   // tiled transpose in flush_cb (0 when not in TILED mode)
    volatile uint32_t flush_us = 0;   // esp_lcd_panel_draw_bitmap in flush_cb (stage -> framebuffer)
    volatile uint32_t refr_px  = 0;   // pixels touched by last LVGL refresh

    // rot_us and flush_us are summed across every flush_cb call in a refresh and
    // latched by the LVGL monitor callback, so they stay comparable to refr_ms even
    // when a refresh dispatches more than one area.
    //
    // The point of flush_us is to split the refresh stage into its two passes:
    //   blit  = refr_ms - rot_us - flush_us   (canvas -> LVGL draw buffer)
    //   flush = flush_us                      (draw buffer -> panel framebuffer)
    // Only the blit half is removable by drawing direct instead of via the canvas,
    // so this ratio decides whether that rewrite is worth doing.

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

// Radar drawing.
//
// updateRadarDisplay() no longer paints. It refreshes the HUD label widgets and
// invalidates the radar object; the geometry is emitted from radarDrawEventCb()
// during LVGL's own refresh, straight into the draw buffer.
//
// The individual draw helpers are internal to navigation.cpp — they take an
// lv_draw_ctx_t that only exists inside a draw event, so there is nothing a
// caller outside that context could pass them.
void updateRadarDisplay();

// LV_EVENT_DRAW_MAIN handler for the radar object. Registered by ui_manager when
// the radar screen is built.
void radarDrawEventCb(lv_event_t* e);

// LV_EVENT_DRAW_MAIN_BEGIN handler — fires before LVGL paints the object's
// background, so the two together bracket the background fill.
void radarDrawBeginCb(lv_event_t* e);

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