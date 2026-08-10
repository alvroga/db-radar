#pragma once
#include "core/arduino_compat.h"

namespace rtc {

struct Time {
    int year;   // 2000..2099
    int month;  // 1..12
    int day;    // 1..31
    int hour;   // 0..23
    int minute; // 0..59
    int second; // 0..59
    int wday;   // 0..6 (0=Sun)
    bool valid; // true if plausible & marked-initialized
};

// Init — no TwoWire parameter (i2c_manager handles bus internally)
bool begin(uint8_t i2c_addr = 0x51);

bool read(Time& t);
bool set(const Time& t);
bool set_from_compile_time();
bool set_from_epoch(uint32_t epoch, int tz_offset_minutes = 0);
bool is_initialized();
void clear_initialized();

} // namespace rtc
