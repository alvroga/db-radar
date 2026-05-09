#pragma once

// ============================================================================
// arduino_compat.h — Thin shim replacing <Arduino.h> for ESP-IDF builds
//
// Provides: millis(), delay(), delayMicroseconds(), constrain(), map(),
//           random(), randomSeed(), min(), max(), abs(), String class,
//           Serial object (USB CDC via TinyUSB), IPAddress class.
//
// Does NOT provide: Wire, WiFi, SD_MMC, HardwareSerial — those have
//                   dedicated ESP-IDF drivers in their own modules.
//
// Usage: replace #include "core/arduino_compat.h" with #include "core/arduino_compat.h"
// ============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

// ESP-IDF core headers used by most modules
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"

// ============================================================================
// Timing
// ============================================================================

static inline uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline uint64_t millis64() {
    return esp_timer_get_time() / 1000ULL;
}

static inline void delay(uint32_t ms) {
    if (ms == 0) return;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static inline void delayMicroseconds(uint32_t us) {
    if (us < 1000) {
        uint64_t start = esp_timer_get_time();
        while ((esp_timer_get_time() - start) < us) { /* busy wait */ }
    } else {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
    }
}

// ============================================================================
// Math helpers
// ============================================================================

template<typename T>
static inline T constrain(T x, T lo, T hi) {
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

template<typename T>
static inline T _arduino_min(T a, T b) { return a < b ? a : b; }

template<typename T>
static inline T _arduino_max(T a, T b) { return a > b ? a : b; }

#ifndef min
#define min(a,b) _arduino_min(a,b)
#endif
#ifndef max
#define max(a,b) _arduino_max(a,b)
#endif
#ifndef abs
#define abs(x) ((x) < 0 ? -(x) : (x))
#endif

static inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ============================================================================
// Random
// ============================================================================

static inline void randomSeed(uint32_t seed) {
    srand(seed);
}

static inline long random(long howbig) {
    if (howbig == 0) return 0;
    return rand() % howbig;
}

static inline long random(long howsmall, long howbig) {
    if (howsmall >= howbig) return howsmall;
    return howsmall + (rand() % (howbig - howsmall));
}

// ============================================================================
// GPIO defines (for files that use HIGH/LOW constants)
// Note: pinMode/digitalWrite/digitalRead are NOT provided.
// HAL files (backlight, button, battery) are fully rewritten to use
// gpio_config() + gpio_set_level() + gpio_get_level() directly.
// ============================================================================

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW  0
#endif

// ============================================================================
// String class — Arduino-compatible, backed by std::string
// ============================================================================

class String {
public:
    String() {}
    String(const char* s) : _s(s ? s : "") {}
    String(const std::string& s) : _s(s) {}
    String(int v)           { char buf[16]; snprintf(buf, sizeof(buf), "%d", v); _s = buf; }
    String(unsigned int v)  { char buf[16]; snprintf(buf, sizeof(buf), "%u", v); _s = buf; }
    String(long v)          { char buf[24]; snprintf(buf, sizeof(buf), "%ld", v); _s = buf; }
    String(unsigned long v) { char buf[24]; snprintf(buf, sizeof(buf), "%lu", v); _s = buf; }
    String(float v, int dec=2)  { char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, (double)v); _s = buf; }
    String(double v, int dec=2) { char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, v); _s = buf; }
    String(char c)          { _s = std::string(1, c); }

    String(const String& o)             : _s(o._s) {}
    String& operator=(const char* s)    { _s = s ? s : ""; return *this; }
    String& operator=(const String& o)  { _s = o._s; return *this; }
    String& operator+=(const String& o) { _s += o._s; return *this; }
    String& operator+=(const char* s)   { if (s) _s += s; return *this; }
    String& operator+=(char c)          { _s += c; return *this; }
    String  operator+(const String& o) const { return String(_s + o._s); }
    String  operator+(const char* s)   const { return String(_s + (s ? s : "")); }
    bool    operator==(const String& o) const { return _s == o._s; }
    bool    operator==(const char* s)   const { return _s == (s ? s : ""); }
    bool    operator!=(const String& o) const { return _s != o._s; }
    bool    operator!=(const char* s)   const { return _s != (s ? s : ""); }
    char    operator[](size_t i) const { return (i < _s.size()) ? _s[i] : '\0'; }

    const char* c_str() const { return _s.c_str(); }
    size_t length() const { return _s.size(); }
    bool isEmpty() const  { return _s.empty(); }
    bool empty() const    { return _s.empty(); }

    int indexOf(const String& s, size_t from = 0) const {
        auto pos = _s.find(s._s, from);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(char c, size_t from = 0) const {
        auto pos = _s.find(c, from);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }
    int indexOf(const char* s, size_t from = 0) const {
        if (!s) return -1;
        auto pos = _s.find(s, from);
        return (pos == std::string::npos) ? -1 : (int)pos;
    }

    String substring(size_t from, size_t to = std::string::npos) const {
        if (from > _s.size()) return String();
        return String(_s.substr(from, to == std::string::npos ? std::string::npos : to - from));
    }

    int toInt() const { return atoi(_s.c_str()); }
    float toFloat() const { return (float)atof(_s.c_str()); }
    double toDouble() const { return atof(_s.c_str()); }

    String toLowerCase() const {
        std::string r = _s;
        for (char& c : r) c = tolower((unsigned char)c);
        return String(r);
    }
    String toUpperCase() const {
        std::string r = _s;
        for (char& c : r) c = toupper((unsigned char)c);
        return String(r);
    }

    void trim() {
        size_t start = _s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { _s.clear(); return; }
        size_t end = _s.find_last_not_of(" \t\r\n");
        _s = _s.substr(start, end - start + 1);
    }

    bool startsWith(const String& prefix) const {
        return _s.rfind(prefix._s, 0) == 0;
    }
    bool startsWith(const char* prefix) const {
        if (!prefix) return false;
        return _s.rfind(prefix, 0) == 0;
    }
    bool endsWith(const String& suffix) const {
        if (suffix._s.size() > _s.size()) return false;
        return _s.compare(_s.size() - suffix._s.size(), suffix._s.size(), suffix._s) == 0;
    }

    void reserve(size_t n) { _s.reserve(n); }
    void clear() { _s.clear(); }

    operator std::string() const { return _s; }
    std::string& stdStr() { return _s; }
    const std::string& stdStr() const { return _s; }

private:
    std::string _s;
};

inline String operator+(const char* lhs, const String& rhs) {
    return String(lhs) + rhs;
}

// ============================================================================
// IPAddress — used in wifi_manager public API
// ============================================================================

class IPAddress {
public:
    IPAddress() : _addr(0) {}
    IPAddress(uint32_t addr) : _addr(addr) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : _addr(((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | a) {}

    bool operator==(const IPAddress& o) const { return _addr == o._addr; }
    bool operator!=(const IPAddress& o) const { return _addr != o._addr; }

    String toString() const {
        char buf[16];
        uint8_t* b = (uint8_t*)&_addr;
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return String(buf);
    }
    uint32_t toUInt32() const { return _addr; }
    operator uint32_t() const { return _addr; }

private:
    uint32_t _addr;
};

// ============================================================================
// Serial object — USB CDC via TinyUSB
// Declaration here; defined in src/core/arduino_compat.cpp
// ============================================================================

class SerialClass {
public:
    void begin(uint32_t baud = 115200);  // No-op in ESP-IDF (CDC always on)

    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    void print(const char* s);
    void print(const String& s);
    void print(int v, int base = 10);
    void print(unsigned int v, int base = 10);
    void print(long v, int base = 10);
    void print(unsigned long v, int base = 10);
    void print(float v, int dec = 2);
    void print(double v, int dec = 2);
    void print(char c);

    void println(const char* s = "");
    void println(const String& s);
    void println(int v, int base = 10);
    void println(unsigned int v, int base = 10);
    void println(long v, int base = 10);
    void println(unsigned long v, int base = 10);
    void println(float v, int dec = 2);
    void println(double v, int dec = 2);

    int available();
    int read();
    int peek();
    String readString();
    String readStringUntil(char terminator);
    void flush();

    operator bool() const { return true; }
};

extern SerialClass Serial;

// ============================================================================
// PROGMEM — no-op on ESP32 (data in Flash/RAM automatically)
// ============================================================================
#ifndef PROGMEM
#define PROGMEM
#endif
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))

// ============================================================================
// Arduino type aliases
// ============================================================================
typedef uint8_t  byte;
typedef uint16_t word;
typedef bool     boolean;

// ============================================================================
// CPU frequency (stub — real ESP-IDF PM integration omitted for initial port)
// ============================================================================
static inline void setCpuFrequencyMhz(uint32_t mhz) { (void)mhz; }

// ============================================================================
// Firmware version — generated by scripts/gen_version.py on every build.
// include/core/fw_version_gen.h is overwritten at build time; the committed
// copy is a fallback for IDEs (clangd) that don't run the build script.
// ============================================================================
#include "fw_version_gen.h"
