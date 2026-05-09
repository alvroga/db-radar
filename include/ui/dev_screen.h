#pragma once

#include <lvgl.h>

namespace dev_screen {
    void create();
    void open();
    void close();

    // Tab creation function
    void createDevTab(lv_obj_t* parent);

    // Update functions
    void updateLoggerStatus();
    void updateNTPStatus();
}
