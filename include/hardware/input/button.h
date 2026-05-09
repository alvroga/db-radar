#pragma once
#include "core/arduino_compat.h"
#include <functional>

namespace button {

// Button event types
enum class Event {
    NONE,
    SINGLE_PRESS,      // Single quick press
    DOUBLE_PRESS,      // Two quick presses
    LONG_PRESS,        // Held for 2 seconds (Settings)
    EXTRA_LONG_PRESS,  // Held for 4 seconds (Standby)
    RELEASED           // Button released (after any press type)
};

// Button configuration
struct Config {
    uint32_t debounce_ms = 20;              // Debounce time (20ms: safe for tactile switches)
    uint32_t long_press_ms = 2000;          // Long press threshold (Settings)
    uint32_t extra_long_press_ms = 4000;    // Extra-long press threshold (Standby)
    uint32_t double_press_window_ms = 500;  // Time window for double press detection
};

// Button state
struct State {
    bool current_state = true;              // Current pin state (HIGH = not pressed, active LOW)
    bool last_state = true;                 // Last stable state
    uint32_t last_change_ms = 0;            // Last state change time
    uint32_t press_start_ms = 0;            // When button was pressed
    uint32_t last_release_ms = 0;           // When button was released
    uint8_t press_count = 0;                // Press count for double-press detection
    bool long_press_triggered = false;      // Long press already fired (2s)
    bool extra_long_press_triggered = false; // Extra-long press already fired (4s)
};

// Event callback type
using EventCallback = std::function<void(Event)>;

// Initialize button on specified GPIO (active LOW with pull-up)
bool begin(gpio_num_t pin, const Config& config = Config{});

// Update button state (call frequently from loop/task)
void update();

// Register event callback
void setEventCallback(EventCallback callback);

// Get current button state (for polling if needed)
bool isPressed();

// Get last detected event (cleared after read)
Event getLastEvent();

// Debug: print button statistics
void printStats();

} // namespace button
