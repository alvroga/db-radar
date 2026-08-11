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
    // Which physical GPS module to talk to — see settings_manager's gps_module_type/
    // gps_module_configured (pinned via the one-time first-boot picker or Settings > GPS).
    // BN880_NMEA reuses the exact same NMEA/PAIR parsing path as LC76G_NMEA (no separate
    // protocol handler) — the value exists to drive compass chip selection
    // (device_manager::initCompass()) and UI labeling, not a new GPS parser.
    enum class GpsModule : uint8_t { BH880_UBX = 0, LC76G_NMEA = 1, BN880_NMEA = 2 };

    // Single source of truth for settings_manager's persisted gps_module_type -> GpsModule
    // mapping, and its display name. Every call site that used to inline this as a ternary
    // (device_manager.cpp, main.cpp's picker, diagnostics.cpp) shares these instead — a
    // duplicated inline mapping is exactly what caused beginWithProtocol() to misclassify
    // BN880_NMEA as UBX before this existed (field-caught 2026-08-11). Unrecognized values
    // fall back to BH880_UBX/"BH-880 (UBX)".
    GpsModule moduleFromType(uint8_t type);
    const char* moduleTypeName(uint8_t type);

    // Start GPS on ESP-IDF UART driver (UART1, RX=GPIO44, TX=GPIO43 by default)
    // If baud=0, auto-detects both baud rate AND protocol (UBX vs NMEA/PAIR) — see
    // detectBaud(). Slower (up to ~18s worst case) but makes no assumption about
    // which module is wired in.
    void begin(uint32_t baud = 115200, int rxPin = 44, int txPin = 43);

    // Skips protocol detection — the module is already known (pinned via Settings > GPS).
    // Still auto-detects baud within that one protocol (single pass, ~1.5-9s worst case),
    // since baud can vary per unit/config even when the module type is known.
    void beginWithProtocol(GpsModule module, int rxPin = 44, int txPin = 43);

    // Auto-detect baud rate AND protocol. Returns detected rate or 0 if failed.
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

    // Protocol actually detected on the wire (set by detectBaud()/begin(), or as soon as
    // read() parses a first valid frame if baud was passed explicitly). Lets callers tell a
    // BH-880 (UBX binary) apart from an LC76G or other NMEA/PAIR module without hardcoding
    // module identity — see docs/peripherals.md.
    bool isNmeaProtocol();
    const char* protocolName();
}
