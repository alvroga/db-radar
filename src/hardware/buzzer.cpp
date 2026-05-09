#include "hardware/buzzer.h"
#include "hardware/i2c/i2c_manager.h"
#include "settings_manager.h"

namespace buzzer {

// Beep pattern states
enum class PatternState {
    IDLE,
    CHIRP_ON,      // First short beep
    CHIRP_PAUSE,   // Pause between beeps
    MAIN_BEEP      // Main beep
};

// Module state
static struct {
    bool initialized = false;
    bool button_sound_enabled = false;
    bool buzzer_on = false;
    uint32_t off_time_ms = 0;  // When to turn off (0 = not scheduled)
    i2c_manager::exio::State exio_state;

    // Pattern state machine
    PatternState pattern_state = PatternState::IDLE;
    uint32_t pattern_next_ms = 0;   // When to transition to next state
    uint16_t main_beep_duration = 0; // Duration for main beep

    // Sonar mode (autonomous rhythmic beeping, driven by buzzer::update() at 10ms)
    bool sonar_active = false;
    uint32_t sonar_interval_ms = 0;       // 0 = silent, >0 = ms between beep starts
    uint16_t sonar_beep_duration_ms = 30;  // Duration of each beep
    uint32_t sonar_next_beep_ms = 0;       // When to fire next beep

    // Async rapid pulse (button feedback — non-blocking replacement for rapidPulse())
    uint8_t rapid_remaining = 0;   // Pulses still to fire (0 = inactive)
    bool rapid_on_phase = false;   // true = currently in ON half of a pulse
    uint32_t rapid_next_ms = 0;    // When to toggle next
} g_state;

bool init() {
    if (g_state.initialized) {
        return true;
    }

    Serial.println("[BUZZER] Initializing buzzer subsystem...");

    // Get the EXIO state (should already be initialized by device_manager)
    // We'll just make sure we can access it
    g_state.buzzer_on = false;
    g_state.off_time_ms = 0;

    // Load settings
    reloadSettings();

    g_state.initialized = true;
    Serial.printf("[BUZZER] Initialized (button sound: %s)\n",
                  g_state.button_sound_enabled ? "ON" : "OFF");

    return true;
}

void reloadSettings() {
    const auto& settings = settings_manager::getSettings();
    g_state.button_sound_enabled = settings.button_sound_enabled;
    Serial.printf("[BUZZER] Settings reloaded (button sound: %s)\n",
                  g_state.button_sound_enabled ? "ON" : "OFF");
}

bool isButtonSoundEnabled() {
    return g_state.button_sound_enabled;
}

void on() {
    if (!g_state.initialized) return;

    // Read current EXIO state (i2c_manager handles mutex internally)
    uint8_t current_output;
    if (!i2c_manager::exio::readOutput(current_output)) {
        return;
    }

    // Set buzzer pin HIGH (active high)
    g_state.exio_state.out = current_output;
    if (i2c_manager::exio::set(i2c_manager::exio::BUZZER, true, g_state.exio_state)) {
        g_state.buzzer_on = true;
    }
}

void off() {
    if (!g_state.initialized) return;

    // Read current EXIO state (i2c_manager handles mutex internally)
    uint8_t current_output;
    if (!i2c_manager::exio::readOutput(current_output)) {
        return;
    }

    // Set buzzer pin LOW
    g_state.exio_state.out = current_output;
    if (i2c_manager::exio::set(i2c_manager::exio::BUZZER, false, g_state.exio_state)) {
        g_state.buzzer_on = false;
        g_state.off_time_ms = 0;
    }
}

void chirp(uint16_t duration_ms) {
    if (!g_state.initialized) return;

    on();
    g_state.off_time_ms = millis() + duration_ms;
}

void beep(uint16_t duration_ms) {
    if (!g_state.initialized) return;

    on();
    g_state.off_time_ms = millis() + duration_ms;
}

void doubleBeep() {
    // Pleasant double-beep pattern: chirp + pause + main beep
    patternBeep(150);  // 150ms main beep after the chirp
}

void patternBeep(uint16_t main_duration_ms) {
    if (!g_state.initialized) return;

    // Start the pattern: chirp (80ms) -> pause (60ms) -> main beep
    // This creates a pleasant "dit-dah" sound rather than a single tone
    g_state.pattern_state = PatternState::CHIRP_ON;
    g_state.main_beep_duration = main_duration_ms;
    g_state.pattern_next_ms = millis() + 20;  // Chirp duration: 20ms
    g_state.off_time_ms = 0;  // Clear simple beep timer
    on();
}

void rapidPulse() {
    if (!g_state.initialized) return;

    // 3x rapid pulses: 10ms on, 10ms off (bz 21 pattern)
    // Blocking call for simplicity - total ~60ms
    for (int i = 0; i < 3; i++) {
        on();
        delay(10);
        off();
        delay(10);
    }
}

void rapidPulseAsync() {
    if (!g_state.initialized) return;
    // Skip if already running
    if (g_state.rapid_remaining > 0) return;

    // Only set state flags here — do NOT call on() from UI Task.
    // I2C Task owns all buzzer I2C operations; it will fire the first pulse
    // on its next update() call (within 20ms), which is imperceptible.
    // Starting in OFF phase with rapid_next_ms=0 causes update() to fire ON immediately.
    g_state.rapid_remaining = 3;
    g_state.rapid_on_phase = false;  // update() will flip to ON on first call
    g_state.rapid_next_ms = 0;       // 0 = fire immediately
}

void setSonarInterval(uint32_t interval_ms, uint16_t beep_duration_ms) {
    if (interval_ms == 0) { stopSonar(); return; }
    g_state.sonar_interval_ms = interval_ms;
    g_state.sonar_beep_duration_ms = beep_duration_ms;
    if (!g_state.sonar_active) {
        g_state.sonar_active = true;
        g_state.sonar_next_beep_ms = millis(); // Beep immediately on activation
    }
}

void stopSonar() {
    g_state.sonar_active = false;
    g_state.sonar_interval_ms = 0;
    g_state.sonar_next_beep_ms = 0;
}

bool isSonarActive() { return g_state.sonar_active; }

void update() {
    if (!g_state.initialized) return;

    uint32_t now = millis();

    // Async rapid pulse (button feedback) — highest priority, driven by I2C Task at 20ms
    if (g_state.rapid_remaining > 0) {
        if (now >= g_state.rapid_next_ms) {
            if (g_state.rapid_on_phase) {
                // End of ON phase: turn off, decrement counter
                off();
                g_state.rapid_on_phase = false;
                g_state.rapid_remaining--;
                if (g_state.rapid_remaining > 0) {
                    g_state.rapid_next_ms = now + 20;  // 20ms OFF gap before next pulse
                }
            } else {
                // End of OFF phase: start next ON pulse
                on();
                g_state.rapid_on_phase = true;
                g_state.rapid_next_ms = now + 20;  // 20ms ON duration
            }
        }
        return;  // Rapid pulse owns the buzzer until all pulses are done
    }

    // Sonar mode: autonomous rhythmic beeping (10ms precision)
    // Pattern beeps (button feedback) take priority over sonar
    if (g_state.sonar_active && g_state.sonar_interval_ms > 0
        && g_state.pattern_state == PatternState::IDLE) {
        if (now >= g_state.sonar_next_beep_ms) {
            on();
            g_state.off_time_ms = now + g_state.sonar_beep_duration_ms;
            g_state.sonar_next_beep_ms = now + g_state.sonar_interval_ms;
        }
    }

    // Handle pattern state machine
    if (g_state.pattern_state != PatternState::IDLE && now >= g_state.pattern_next_ms) {
        switch (g_state.pattern_state) {
            case PatternState::CHIRP_ON:
                // Chirp done, start pause
                off();
                g_state.pattern_state = PatternState::CHIRP_PAUSE;
                g_state.pattern_next_ms = now + 60;  // Pause: 60ms
                break;

            case PatternState::CHIRP_PAUSE:
                // Pause done, start main beep
                on();
                g_state.pattern_state = PatternState::MAIN_BEEP;
                g_state.pattern_next_ms = now + g_state.main_beep_duration;
                break;

            case PatternState::MAIN_BEEP:
                // Main beep done, return to idle
                off();
                g_state.pattern_state = PatternState::IDLE;
                break;

            default:
                break;
        }
        return;  // Pattern handled, skip simple beep check
    }

    // Check if we need to turn off the buzzer (simple beep mode)
    if (g_state.off_time_ms > 0 && now >= g_state.off_time_ms) {
        off();
    }
}

} // namespace buzzer
