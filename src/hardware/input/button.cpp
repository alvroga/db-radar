#include "hardware/input/button.h"
#include "hardware/buzzer.h"
#include "utils/standby_manager.h"
#include "core/arduino_compat.h"
#include "driver/gpio.h"

namespace button {

// Module state
static gpio_num_t s_pin = GPIO_NUM_NC;
static Config s_config;
static State s_state;
static EventCallback s_callback = nullptr;
static Event s_last_event = Event::NONE;

// Boot safety: GPIO0 is the ESP32 bootloader button
// Delay button activation to avoid boot-time issues
static constexpr uint32_t BOOT_DELAY_MS = 5000;  // 5 seconds (matches loading screen)
static uint32_t s_boot_time_ms = 0;
static bool s_boot_delay_active = true;

// Statistics
static struct {
    uint32_t total_presses = 0;
    uint32_t single_presses = 0;
    uint32_t double_presses = 0;
    uint32_t long_presses = 0;
    uint32_t boot_ignored = 0;  // Presses ignored during boot delay
} s_stats;

bool begin(gpio_num_t pin, const Config& config) {
    s_pin = pin;
    s_config = config;

    // Configure GPIO as input with internal pull-up (button is active LOW)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // Record boot time for safety delay
    s_boot_time_ms = millis();
    s_boot_delay_active = true;

    // Initialize state
    s_state = State{};
    s_state.current_state = gpio_get_level(pin);
    s_state.last_state = s_state.current_state;
    s_state.last_change_ms = millis();

    Serial.printf("[BUTTON] Initialized on GPIO%d (active LOW, pull-up enabled)\n", pin);
    Serial.printf("[BUTTON] Config: debounce=%ums, long_press=%ums, double_window=%ums\n",
                  s_config.debounce_ms, s_config.long_press_ms, s_config.double_press_window_ms);

    // GPIO0 boot safety check
    if (pin == GPIO_NUM_0) {
        Serial.printf("[BUTTON] GPIO0 boot safety: %ums delay before activation\n", BOOT_DELAY_MS);

        // Check if GPIO0 is stuck LOW (bootloader mode or hardware issue)
        if (s_state.current_state == LOW) {
            Serial.println("[BUTTON] ⚠️ WARNING: GPIO0 is LOW at boot - may indicate bootloader mode or stuck button");
            Serial.printf("[BUTTON] If button doesn't work after %lus, try power cycle (not just reset)\n", BOOT_DELAY_MS / 1000UL);
        }
    }

    return true;
}

void update() {
    if (s_pin == GPIO_NUM_NC) return;

    uint32_t now = millis();

    // Boot delay: ignore button events for first 5 seconds after boot
    // This prevents GPIO0 bootloader issues and gives system time to stabilize
    if (s_boot_delay_active) {
        if ((now - s_boot_time_ms) >= BOOT_DELAY_MS) {
            s_boot_delay_active = false;
            Serial.println("[BUTTON] ✓ Boot delay complete - button now active");

            // CRITICAL FIX: If button is currently held when delay ends, ignore it
            // This prevents the "first press takes 8 seconds" bug where:
            // - User presses button at T+3s (during boot delay)
            // - Boot delay ends at T+5s
            // - Button system sees it as a "new press" starting at T+5s
            // - User must hold until T+7s to reach 2s long press threshold
            bool current_reading = gpio_get_level(s_pin);
            if (current_reading == LOW) {
                Serial.println("[BUTTON] Button held during boot delay - ignoring this press");
                // Initialize state to reflect button is already pressed (prevents false "press" event)
                s_state.current_state = LOW;
                s_state.last_state = LOW;
                s_state.last_change_ms = now;
                return; // Skip this update cycle - wait for button release
            }
        } else {
            // Still in boot delay - ignore all button activity
            bool current_reading = gpio_get_level(s_pin);
            if (current_reading == LOW) {
                s_stats.boot_ignored++;
            }
            return;
        }
    }

    bool current_reading = gpio_get_level(s_pin);

    // Debouncing: wait for stable state
    if (current_reading != s_state.current_state) {
        s_state.current_state = current_reading;
        s_state.last_change_ms = now;
        return; // Wait for stability
    }

    // Check if state is stable (debounced)
    if ((now - s_state.last_change_ms) < s_config.debounce_ms) {
        return; // Still bouncing
    }

    // State is stable, check for transitions
    bool pressed = (s_state.current_state == LOW); // Active LOW
    bool was_pressed = (s_state.last_state == LOW);

    // === BUTTON PRESSED (transition HIGH -> LOW) ===
    if (pressed && !was_pressed) {
        standby_manager::notifyUserActivity();  // Reset inactivity timer
        s_state.press_start_ms = now;
        s_state.long_press_triggered = false;
        s_state.extra_long_press_triggered = false;
        s_state.press_count++;
        s_stats.total_presses++;

        // Detect double press IMMEDIATELY on 2nd press down — no 400ms wait after release.
        // Gap is measured from last release to this press, must be within double_press_window_ms.
        if (s_state.press_count == 2) {
            uint32_t gap = now - s_state.last_release_ms;
            if (gap <= s_config.double_press_window_ms) {
                s_last_event = Event::DOUBLE_PRESS;
                s_stats.double_presses++;
                s_state.press_count = 0;  // Clear so timeout block doesn't re-fire
                Serial.println("[BUTTON] Double press detected");
                if (s_callback) s_callback(Event::DOUBLE_PRESS);
            } else {
                // Gap too wide — treat this as a fresh first press
                s_state.press_count = 1;
            }
        }

        // Request 3x rapid pulses (non-blocking — I2C Task drives timing via buzzer::update())
        if (buzzer::isButtonSoundEnabled()) {
            buzzer::rapidPulseAsync();
        }

        Serial.printf("[BUTTON] Pressed (count=%d)\n", s_state.press_count);
    }

    // === BUTTON HELD (check for long press) ===
    if (pressed && was_pressed) {
        uint32_t hold_duration = now - s_state.press_start_ms;

        // Trigger extra-long press once (4 seconds - Standby)
        if (!s_state.extra_long_press_triggered && hold_duration >= s_config.extra_long_press_ms) {
            s_state.extra_long_press_triggered = true;
            s_state.long_press_triggered = true;  // Also mark long press as fired
            s_state.press_count = 0; // Clear press count
            s_last_event = Event::EXTRA_LONG_PRESS;
            s_stats.long_presses++;  // Count as long press stat

            Serial.println("[BUTTON] Extra-long press detected (4s) - Standby mode");

            if (s_callback) {
                s_callback(Event::EXTRA_LONG_PRESS);
            }
        }
        // Trigger long press once (2 seconds - Settings)
        else if (!s_state.long_press_triggered && hold_duration >= s_config.long_press_ms) {
            s_state.long_press_triggered = true;
            s_state.press_count = 0; // Clear press count
            s_last_event = Event::LONG_PRESS;
            s_stats.long_presses++;

            Serial.println("[BUTTON] Long press detected (2s) - Settings");

            if (s_callback) {
                s_callback(Event::LONG_PRESS);
            }
        }
    }

    // === BUTTON RELEASED (transition LOW -> HIGH) ===
    if (!pressed && was_pressed) {
        s_state.last_release_ms = now;
        uint32_t press_duration = now - s_state.press_start_ms;

        Serial.printf("[BUTTON] Released (duration=%ums)\n", press_duration);

        // Only count as press if it wasn't a long press
        if (!s_state.long_press_triggered) {
            // Don't immediately trigger single press - wait for possible double press
            // (handled in next update cycle)
        }

        // Always trigger RELEASED event
        s_last_event = Event::RELEASED;
        if (s_callback) {
            s_callback(Event::RELEASED);
        }
    }

    // === SINGLE PRESS TIMEOUT ===
    // Double press is now detected instantly on 2nd press down.
    // This block only handles single press confirmation (waits for window to expire
    // to confirm no second press is coming).
    if (!pressed && s_state.press_count > 0) {
        uint32_t time_since_release = now - s_state.last_release_ms;

        if (time_since_release > s_config.double_press_window_ms) {
            if (s_state.press_count == 1 && !s_state.long_press_triggered) {
                s_last_event = Event::SINGLE_PRESS;
                s_stats.single_presses++;
                Serial.println("[BUTTON] Single press confirmed");
                if (s_callback) s_callback(Event::SINGLE_PRESS);
            }
            s_state.press_count = 0;
        }
    }

    // Update last state
    s_state.last_state = s_state.current_state;
}

void setEventCallback(EventCallback callback) {
    s_callback = callback;
    Serial.println("[BUTTON] Event callback registered");
}

bool isPressed() {
    if (s_pin == GPIO_NUM_NC) return false;
    return gpio_get_level(s_pin) == LOW;
}

Event getLastEvent() {
    Event event = s_last_event;
    s_last_event = Event::NONE;
    return event;
}

void printStats() {
    Serial.println("==== Button Statistics ====");
    Serial.printf("Total presses:    %u\n", s_stats.total_presses);
    Serial.printf("Single presses:   %u\n", s_stats.single_presses);
    Serial.printf("Double presses:   %u\n", s_stats.double_presses);
    Serial.printf("Long presses:     %u\n", s_stats.long_presses);
    Serial.printf("Current state:    %s\n", isPressed() ? "PRESSED" : "RELEASED");
    Serial.println("===========================");
}

} // namespace button
