#pragma once

#include <lvgl.h>

namespace settings_screen {
    void create();
    void open();
    void close();

    // Tab creation functions
    void createGPSTab(lv_obj_t* parent);
    void createWiFiTab(lv_obj_t* parent);
    void createDisplayTab(lv_obj_t* parent);
    void createSoundTab(lv_obj_t* parent);

    // Helper functions
    void updateWiFiStatus();
    void updateAPModeStatus();
}
