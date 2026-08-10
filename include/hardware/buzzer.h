#ifndef BUZZER_H
#define BUZZER_H

#include "core/arduino_compat.h"

namespace buzzer {

/**
 * @brief Initialize the buzzer subsystem
 * Must be called after i2c_manager::init()
 * @return true on success
 */
bool init();

/**
 * @brief Play a short chirp sound (for button press feedback)
 * Non-blocking: starts the chirp and schedules auto-off
 * @param duration_ms Duration in milliseconds (default: 30ms)
 */
void chirp(uint16_t duration_ms = 30);

/**
 * @brief Play a beep sound (for alerts/notifications)
 * @param duration_ms Duration in milliseconds (default: 100ms)
 */
void beep(uint16_t duration_ms = 100);

/**
 * @brief Play a double beep (for confirmations)
 * Pattern: chirp (50ms) + pause (50ms) + main beep (150ms)
 */
void doubleBeep();

/**
 * @brief Play a pleasant pattern beep: chirp + pause + main beep
 * More pleasant than a single tone
 * @param main_duration_ms Duration of the main beep portion (default: 150ms)
 */
void patternBeep(uint16_t main_duration_ms = 150);

/**
 * @brief Request 3x rapid pulses asynchronously (non-blocking)
 * State is set here; actual I2C toggling is driven by update() in the I2C Task.
 *
 * This is the only rapid-pulse entry point. A blocking rapidPulse() that spun in
 * delay() for ~60ms was removed 2026-07-31 — it had no callers left and calling it
 * from the UI Task would have stalled LVGL for most of a frame.
 */
void rapidPulseAsync();

/**
 * @brief Turn buzzer on
 */
void on();

/**
 * @brief Turn buzzer off
 */
void off();

/**
 * @brief Check if button sound is enabled in settings
 * @return true if enabled
 */
bool isButtonSoundEnabled();

/**
 * @brief Reload button sound setting from NVS
 * Call this after settings change
 */
void reloadSettings();

/**
 * @brief Update buzzer state (call from main loop/task to handle timing)
 */
void update();

/**
 * @brief Set autonomous sonar beeping interval (driven by update() at 10ms precision)
 * @param interval_ms Milliseconds between beep starts (0 = stop)
 * @param beep_duration_ms Duration of each beep (default: 30ms)
 */
void setSonarInterval(uint32_t interval_ms, uint16_t beep_duration_ms = 30);

/**
 * @brief Stop sonar beeping
 */
void stopSonar();

/**
 * @brief Check if sonar mode is active
 */
bool isSonarActive();

} // namespace buzzer

#endif // BUZZER_H
