#pragma once
#include "core/arduino_compat.h"

namespace backlight {

struct Cfg {
  int pin = 6;
  int ledcChan = 0;     // 0..7
  int ledcTimer = 0;    // 0..3
  uint32_t freqHz = 20000; // 20 kHz (quiet)
  int resBits = 8;      // valid: 1..20 (we clamp)
  bool usePwm = true;   // set false to force simple on/off
};

bool begin(const Cfg &cfg);
void set(uint8_t level);  // 0..255
void setPercent(uint8_t percent);  // 0..100
uint8_t getPercent();  // Returns current brightness as 0..100%
void on();
void off();

} // namespace backlight