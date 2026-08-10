#pragma once

#include "esp_wifi_types.h"

// =============================================================================
// CONFIGURATION
// =============================================================================

// Note: BLE scanning is handled independently by beacon_proximity.cpp

// =============================================================================
// PUBLIC API
// =============================================================================

namespace scanner {
    void init();
    void update();

    int getWiFiCount();

    void setWiFiEnabled(bool enabled);

    bool isWiFiEnabled();

    // Manually trigger a WiFi scan (for Settings UI)
    void triggerWiFiScan();

    // Scan result access (for Settings UI network list)
    // Returns: -1 = scan running, -2 = failed/not started, >=0 = AP count
    static constexpr int MAX_SCAN_APS = 20;
    static constexpr int SCAN_RUNNING = -1;
    static constexpr int SCAN_FAILED  = -2;

    struct APRecord {
        char ssid[33];
        int8_t rssi;
        wifi_auth_mode_t authmode;
    };

    int scanComplete();
    const APRecord* getScanRecord(int index);
}