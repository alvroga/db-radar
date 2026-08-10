#pragma once
#include <lvgl.h>

// DEV-mode screen for capturing compass/accel/GPS field samples.
//
// ⚠️ LVGL IS NOT THREAD-SAFE. Everything here runs on the UI Task only. Violating
// that hangs the UI Task at loop count 2 and trips the watchdog — see project
// memory. Nothing in this file may be called from the System Task.

namespace field_log_screen {

void create();
void open();
void close();

} // namespace field_log_screen
