#ifndef BOOT_BUTTON_H
#define BOOT_BUTTON_H

#include "core/arduino_compat.h"
#include <functional>

/**
 * @file boot_button.h
 * @brief Boot button (GPIO0) input handler with debouncing and event detection
 *
 * Provides access to the hardware BOOT button (GPIO0) as a user input device.
 * After ESP32-S3 boots, GPIO0 can be used as a regular GPIO for button input.
 *
 * Features:
 * - Short press detection (< 2 seconds)
 * - Long press detection (>= 2 seconds)
 * - Double press detection (two presses within 500ms)
 * - Proper debouncing (50ms)
 * - Event callback system
 *
 * Usage in cc-radar:
 * - Long press: Open/close settings menu
 * - Short press: Future use (zoom, etc.)
 * - Double press: Future use (waypoint actions, etc.)
 */

namespace boot_button {

// Button event types
enum class Event {
    NONE,
    SHORT_PRESS,      // Button pressed and released quickly (< 2s)
    LONG_PRESS,       // Button held for >= 2 seconds
    DOUBLE_PRESS,     // Two quick presses within 500ms
    PRESS_START,      // Button just pressed (for immediate feedback)
    PRESS_END         // Button just released
};

// Event callback type
using EventCallback = std::function<void(Event)>;

// Configuration
struct Config {
    int pin = 0;                          // GPIO pin (default: GPIO0)
    bool active_low = true;               // Active LOW (button pulls to GND)
    uint32_t debounce_ms = 50;            // Debounce time in milliseconds
    uint32_t long_press_ms = 2000;        // Long press threshold (2 seconds)
    uint32_t double_press_window_ms = 500; // Double press time window
};

/**
 * @brief Initialize boot button
 * @param config Button configuration
 * @return true if successful, false on error
 */
bool init(const Config& config = Config());

/**
 * @brief Update button state (call from main loop or timer)
 * Must be called regularly (recommended: every 10-20ms)
 */
void update();

/**
 * @brief Register event callback
 * @param callback Function to call when button event occurs
 */
void setEventCallback(EventCallback callback);

/**
 * @brief Get current button state (debounced)
 * @return true if button is currently pressed, false if released
 */
bool isPressed();

/**
 * @brief Get time button has been held (milliseconds)
 * @return Milliseconds button has been held, 0 if not pressed
 */
uint32_t getPressedDuration();

/**
 * @brief Get last event
 * @return Last detected button event
 */
Event getLastEvent();

/**
 * @brief Clear last event (useful for one-shot event handling)
 */
void clearLastEvent();

/**
 * @brief Get button statistics for debugging
 */
struct Stats {
    uint32_t short_press_count;
    uint32_t long_press_count;
    uint32_t double_press_count;
    uint32_t total_presses;
};

Stats getStats();

/**
 * @brief Reset statistics counters
 */
void resetStats();

} // namespace boot_button

#endif // BOOT_BUTTON_H
