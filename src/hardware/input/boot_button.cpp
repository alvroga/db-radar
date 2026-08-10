#include "hardware/input/boot_button.h"
#include "driver/gpio.h"

namespace boot_button {

// Global state
static Config g_config;
static EventCallback g_callback = nullptr;
static Stats g_stats = {0, 0, 0, 0};

// Button state tracking
static bool g_current_state = false;       // Current debounced state
static bool g_last_raw_state = false;      // Last raw GPIO reading
static uint32_t g_last_debounce_time = 0;  // Last time state changed
static uint32_t g_press_start_time = 0;    // When button was pressed
static uint32_t g_last_press_time = 0;     // Time of last press release
static bool g_long_press_fired = false;    // Prevent multiple long press events
static Event g_last_event = Event::NONE;

// Forward declarations
static bool readRawState();
static void fireEvent(Event event);

bool init(const Config& config) {
    g_config = config;

    // Configure GPIO as input with pull-up (button pulls to GND when pressed)
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << g_config.pin;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    // Initialize state
    g_current_state = false;
    g_last_raw_state = readRawState();
    g_last_debounce_time = millis();
    g_press_start_time = 0;
    g_last_press_time = 0;
    g_long_press_fired = false;
    g_last_event = Event::NONE;

    Serial.printf("[BOOT_BTN] Initialized on GPIO%d (active %s)\n",
                  g_config.pin,
                  g_config.active_low ? "LOW" : "HIGH");

    return true;
}

void update() {
    uint32_t now = millis();
    bool raw_state = readRawState();

    // Debouncing logic
    if (raw_state != g_last_raw_state) {
        g_last_debounce_time = now;
        g_last_raw_state = raw_state;
    }

    // Check if debounce time has passed
    if ((now - g_last_debounce_time) > g_config.debounce_ms) {
        // Debounced state is stable

        // Check for state change
        if (raw_state != g_current_state) {
            g_current_state = raw_state;

            if (g_current_state) {
                // Button just pressed
                g_press_start_time = now;
                g_long_press_fired = false;
                fireEvent(Event::PRESS_START);

                // Check for double press
                if (g_last_press_time > 0 &&
                    (now - g_last_press_time) < g_config.double_press_window_ms) {
                    fireEvent(Event::DOUBLE_PRESS);
                    g_stats.double_press_count++;
                    g_last_press_time = 0;  // Prevent triple-press detection
                }
            } else {
                // Button just released
                uint32_t press_duration = now - g_press_start_time;
                fireEvent(Event::PRESS_END);

                // Determine press type (only if long press wasn't already fired)
                if (!g_long_press_fired) {
                    if (press_duration >= g_config.long_press_ms) {
                        fireEvent(Event::LONG_PRESS);
                        g_stats.long_press_count++;
                    } else {
                        fireEvent(Event::SHORT_PRESS);
                        g_stats.short_press_count++;
                        g_last_press_time = now;  // Track for double-press detection
                    }
                }

                g_stats.total_presses++;
                g_press_start_time = 0;
            }
        }
    }

    // Check for long press while button is held
    if (g_current_state && !g_long_press_fired) {
        uint32_t press_duration = now - g_press_start_time;
        if (press_duration >= g_config.long_press_ms) {
            fireEvent(Event::LONG_PRESS);
            g_stats.long_press_count++;
            g_long_press_fired = true;  // Fire only once per press
        }
    }
}

void setEventCallback(EventCallback callback) {
    g_callback = callback;
}

bool isPressed() {
    return g_current_state;
}

uint32_t getPressedDuration() {
    if (g_current_state && g_press_start_time > 0) {
        return millis() - g_press_start_time;
    }
    return 0;
}

Event getLastEvent() {
    return g_last_event;
}

void clearLastEvent() {
    g_last_event = Event::NONE;
}

Stats getStats() {
    return g_stats;
}

void resetStats() {
    g_stats = {0, 0, 0, 0};
}

// Private helper functions

static bool readRawState() {
    bool raw = (gpio_get_level((gpio_num_t)g_config.pin) != 0);
    // Invert if active LOW (button pressed = LOW, return true)
    return g_config.active_low ? !raw : raw;
}

static void fireEvent(Event event) {
    g_last_event = event;

    // Debug logging
    const char* event_name = "UNKNOWN";
    switch (event) {
        case Event::SHORT_PRESS:   event_name = "SHORT_PRESS"; break;
        case Event::LONG_PRESS:    event_name = "LONG_PRESS"; break;
        case Event::DOUBLE_PRESS:  event_name = "DOUBLE_PRESS"; break;
        case Event::PRESS_START:   event_name = "PRESS_START"; break;
        case Event::PRESS_END:     event_name = "PRESS_END"; break;
        default: break;
    }

    Serial.printf("[BOOT_BTN] Event: %s\n", event_name);

    // Call user callback
    if (g_callback) {
        g_callback(event);
    }
}

} // namespace boot_button
