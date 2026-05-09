#pragma once
#include "core/arduino_compat.h"

struct GPSData {
    // Position
    double lat = 0.0;
    double lon = 0.0;
    double alt = NAN;
    int    sats = 0;
    float  hdop = NAN;

    // Movement (from UBX NAV-PVT)
    float  course = NAN;
    float  speed = NAN;
    bool   hasHeading = false;

    // Fix flags
    bool   valid = false;

    // UTC time from GPS (UBX NAV-PVT)
    bool   hasTime = false;
    int    year = 0;
    int    month = 0;
    int    day = 0;
    int    hour = 0;
    int    minute = 0;
    int    second = 0;

    // Quality metrics
    uint32_t last_update_ms = 0;
    uint32_t time_to_first_fix_ms = 0;
    float    quality_score = 0.0f;
    bool     position_jump_detected = false;
};

namespace gps_bh880 {
    // Start GPS on ESP-IDF UART driver (UART1, RX=GPIO44, TX=GPIO43 by default)
    // If baud=0, auto-detects baud rate by trying common rates.
    void begin(uint32_t baud = 115200, int rxPin = 44, int txPin = 43);

    // Auto-detect baud rate. Returns detected rate or 0 if failed.
    uint32_t detectBaud(int rxPin, int txPin);

    // Feed and parse UART bytes. Returns true when a full UBX NAV-PVT was parsed.
    bool read(GPSData &out);

    // UBX configuration commands
    bool setUpdateRate(uint32_t intervalMs);
    bool setPowerMode(uint8_t mode, uint16_t periodMs = 0, uint16_t onTimeMs = 0);
    bool setUpdateRateVALSET(uint32_t intervalMs, uint8_t layers = 0x01);
    bool setBaudrate(uint32_t baud);
    bool hotStart();
    bool warmStart();
    bool coldStart();
    bool factoryReset();
    bool saveConfig();

    // Low-level UBX
    bool sendUBX(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);
    void printModuleInfo();
    bool ping(uint32_t timeout_ms = 1000);
    void dumpRaw(uint32_t duration_ms = 5000);
}
