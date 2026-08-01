#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_DEV_VERSION 1

// Memory - must be large enough for all widgets + rotation temp buffer (~10KB)
// Settings screen has ~60 widgets; 64KB provides headroom for draw buffers
#define LV_MEM_SIZE (64 * 1024U)

// Color settings
#define LV_COLOR_DEPTH 16

// Tick — use hardware timer instead of manual lv_tick_inc() calls.
// esp_timer_get_time() returns microseconds since boot; divide by 1000 for ms.
// This eliminates tick jitter caused by display_mutex hold times.
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000LL))

// Display
// 10ms reads like a 100Hz refresh request on a panel that runs at 10MHz/(528x502) = 37.7Hz. It is
// not one, and it is NOT what paces rendering — the UI Task's vsync gate is (task_manager.cpp).
// Verified in LVGL 8.3 source before leaving it alone:
//   - hal/lv_hal_disp.c:195 — this is the period of disp->refr_timer, which only decides how soon
//     after an invalidate a refresh may START. With nothing invalidated it does no work, so the
//     "100Hz" never happens; raising it to 26ms would just add up to 26ms of latency per redraw.
//   - misc/lv_anim.c:59 — the same macro sets the ANIMATION timer period. Changing it here retimes
//     every animation in the UI, which is the real reason not to "fix" this value.
#define LV_DISP_DEF_REFR_PERIOD 10
#define LV_DPI_DEF 130

// Input devices
// LVGL's built-in perf monitor is disabled: it aligns to LV_ALIGN_BOTTOM_RIGHT,
// which is behind the bezel on this round 480x480 panel, so it was never
// visible while still costing a label refresh every 300ms. The DEV perf HUD in
// navigation.cpp replaces it and reports the paint/refresh split we actually
// need (see docs/performance_optimization_backlog.md).
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

// Fonts
#define LV_USE_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_16 1   // Needed for LV_SYMBOL_BATTERY_* glyphs on battery label

// Widgets
#define LV_USE_BTN 1
#define LV_USE_LABEL 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1

// Others
#define LV_USE_LOG 1
#define LV_USE_USER_DATA 1

#endif // LV_CONF_H