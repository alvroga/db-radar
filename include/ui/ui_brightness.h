#pragma once
#include <lvgl.h>

// Creates a simple LVGL screen with a slider and label to control
// LCD backlight brightness using backlight::set().
//
// Usage:
//  - Ensure LVGL is initialized and a display driver is registered.
//  - Ensure backlight::begin() has been called.
//  - Call ui_create_brightness_slider();
//  - Run your normal LVGL task handler loop.
void ui_create_brightness_slider();

