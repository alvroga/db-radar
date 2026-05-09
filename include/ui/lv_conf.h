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
#define LV_DISP_DEF_REFR_PERIOD 10  // 10ms = 100Hz refresh (Waveshare optimization for smooth scrolling)
#define LV_DPI_DEF 130

// Input devices
#define LV_USE_PERF_MONITOR 1
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