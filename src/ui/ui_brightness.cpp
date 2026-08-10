#include "core/arduino_compat.h"
#include <lvgl.h>
#include "backlight.h"
#include "ui_brightness.h"

static lv_obj_t *g_label = nullptr;

static void slider_event_cb(lv_event_t *e) {
  lv_obj_t *slider = lv_event_get_target(e);
  int32_t val = lv_slider_get_value(slider);     // 0..100

  // Map 0..100 to 0..255 for PWM
  uint8_t duty = (uint8_t)((val * 255 + 50) / 100);
  backlight::set(duty);

  if (g_label) {
    lv_label_set_text_fmt(g_label, "Brightness: %d%%", (int)val);
  }
}

void ui_create_brightness_slider() {
  // Create a fresh screen
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Title label
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LVGL Brightness Control");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  // Value label
  g_label = lv_label_create(scr);
  lv_label_set_text(g_label, "Brightness: 100%");
  lv_obj_align(g_label, LV_ALIGN_TOP_MID, 0, 40);

  // Slider
  lv_obj_t *slider = lv_slider_create(scr);
  lv_obj_set_size(slider, lv_pct(80), 24);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -24);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, 100, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Apply initial brightness
  backlight::set(255);

  lv_scr_load(scr);
}

