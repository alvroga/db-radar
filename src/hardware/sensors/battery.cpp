#include "hardware/sensors/battery.h"
#include "core/system_config.h"
#include "core/arduino_compat.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

namespace battery {

// =============================================================================
// INTERNAL CONSTANTS
// =============================================================================

// ESP-IDF ADC handles — GPIO4 = ADC1_CHANNEL_3 on ESP32-S3
static adc_oneshot_unit_handle_t g_adc_handle = nullptr;
static adc_cali_handle_t g_cali_handle = nullptr;
static const adc_unit_t     BAT_ADC_UNIT    = ADC_UNIT_1;
static const adc_channel_t  BAT_ADC_CHANNEL = ADC_CHANNEL_3;  // GPIO4

namespace config {
    // Hardware configuration
    constexpr int BAT_ADC_PIN = 4;              // GPIO4 connected to BAT_ADC
    constexpr float VOLTAGE_DIVIDER = 3.255;    // Calibrated: multimeter 3.83V vs reported 3.53V → 3.0 × (3.83/3.53)
    constexpr float ADC_REFERENCE = 3.3;        // ESP32-S3 ADC reference voltage
    constexpr int ADC_RESOLUTION = 4095;        // 12-bit ADC (0-4095)
    constexpr int ADC_SAMPLES = 15;             // Number of samples for averaging (increased for stability)

    // EMA (Exponential Moving Average) smoothing configuration
    constexpr float EMA_ALPHA = 0.2f;           // Smoothing factor (0.1-0.3 range, lower = smoother)

    // Battery voltage thresholds (calibrated from real discharge test 2026-02-13)
    constexpr float VBAT_MAX = 4.12;            // Fully charged voltage (measured after USB disconnect)
    constexpr float VBAT_MIN = 3.0;             // Empty voltage (cutoff)
    constexpr float VBAT_NOMINAL = 3.7;         // Nominal voltage (~50%)

    // Warning thresholds (adjusted to match real discharge curve)
    constexpr float LOW_BATTERY_VOLTAGE = 3.45;  // ~20% warning threshold
    constexpr float CRITICAL_VOLTAGE = 3.36;     // ~10% critical threshold
    constexpr float SHUTDOWN_VOLTAGE = 3.0;      // 0% emergency shutdown

    constexpr int LOW_BATTERY_PERCENT = 20;
    constexpr int CRITICAL_PERCENT = 10;

    // Charge detection thresholds
    constexpr float USB_DETECT_VOLTAGE = 3.85;   // > 3.85V indicates USB power (lowered from 4.0V for mid-charge detection)
    constexpr float FULL_CHARGE_VOLTAGE = 4.15;  // > 4.15V indicates full charge

    // Warning intervals (milliseconds)
    constexpr unsigned long LOW_BATTERY_WARNING_INTERVAL = 300000;    // 5 minutes
    constexpr unsigned long CRITICAL_WARNING_INTERVAL = 60000;        // 1 minute

    // Monitoring intervals
    constexpr unsigned long MONITORING_INTERVAL = 60000;  // 60 seconds

    // Voltage trend tracking
    constexpr int HISTORY_SIZE = 10;                      // Number of voltage samples to track
    constexpr unsigned long HISTORY_UPDATE_INTERVAL = 30000;  // 30 seconds between samples
    constexpr float VOLTAGE_CHANGE_THRESHOLD = 0.01;      // 10mV minimum change to detect trend (lowered for mid-charge sensitivity)
    constexpr float STABLE_VOLTAGE_THRESHOLD = 0.01;      // 10mV variation for "stable" state
}

// =============================================================================
// INTERNAL STATE
// =============================================================================

namespace state {
    bool initialized = false;
    bool monitoring_enabled = false;
    unsigned long last_warning_time = 0;
    unsigned long last_monitoring_time = 0;

    // Voltage history tracking for trend analysis
    float voltage_history[config::HISTORY_SIZE] = {0};
    int history_index = 0;
    int history_count = 0;
    unsigned long last_history_update = 0;

    // EMA smoothing state
    float smoothed_voltage = 0.0f;              // EMA-smoothed voltage
    bool ema_initialized = false;               // First reading flag
}

// =============================================================================
// INTERNAL HELPER FUNCTIONS
// =============================================================================

/**
 * @brief Update voltage history with current reading
 *
 * Adds voltage sample to circular buffer if update interval has elapsed.
 * Call this regularly to build voltage trend data.
 */
static void updateVoltageHistory(float voltage) {
    unsigned long now = millis();

    // Only update if enough time has passed
    if (state::history_count > 0 &&
        (now - state::last_history_update) < config::HISTORY_UPDATE_INTERVAL) {
        return;
    }

    // Add voltage to circular buffer
    state::voltage_history[state::history_index] = voltage;
    state::history_index = (state::history_index + 1) % config::HISTORY_SIZE;

    if (state::history_count < config::HISTORY_SIZE) {
        state::history_count++;
    }

    state::last_history_update = now;
}

/**
 * @brief Detect battery state from voltage trend analysis
 *
 * Hybrid approach for immediate feedback:
 * - 0-2 samples: Voltage-based heuristic
 * - 3-4 samples: Simple trend (first vs last)
 * - 5+ samples: Full trend analysis (first half vs second half)
 */
static BatteryState detectBatteryState(float current_voltage) {
    // Phase 1: Immediate feedback (0-2 samples) - Voltage heuristic
    if (state::history_count < 3) {
        // Check for full battery first
        if (current_voltage >= config::FULL_CHARGE_VOLTAGE) {
            return BatteryState::FULL;
        }
        // Voltage above 4.0V usually indicates USB power/charging
        else if (current_voltage > config::USB_DETECT_VOLTAGE) {
            return BatteryState::CHARGING;
        }
        // Voltage below 4.0V usually indicates battery power
        else {
            return BatteryState::DISCHARGING;
        }
    }

    // Phase 2: Simple trend (3-4 samples) - First vs Last
    if (state::history_count < 5) {
        float oldest_voltage = state::voltage_history[(state::history_index + config::HISTORY_SIZE - state::history_count) % config::HISTORY_SIZE];
        float voltage_change = current_voltage - oldest_voltage;

        // Check for full battery
        if (current_voltage >= config::FULL_CHARGE_VOLTAGE &&
            fabsf(voltage_change) < config::STABLE_VOLTAGE_THRESHOLD) {
            return BatteryState::FULL;
        }

        // Determine trend from first to last sample
        if (voltage_change > config::VOLTAGE_CHANGE_THRESHOLD) {
            return BatteryState::CHARGING;
        } else if (voltage_change < -config::VOLTAGE_CHANGE_THRESHOLD) {
            return BatteryState::DISCHARGING;
        } else {
            return BatteryState::STABLE;
        }
    }

    // Phase 3: Full trend analysis (5+ samples) - First half vs Second half
    // Check if battery is full (stable at ~4.2V)
    if (current_voltage >= config::FULL_CHARGE_VOLTAGE) {
        float oldest_voltage = state::voltage_history[(state::history_index + config::HISTORY_SIZE - state::history_count) % config::HISTORY_SIZE];
        if (fabsf(current_voltage - oldest_voltage) < config::STABLE_VOLTAGE_THRESHOLD) {
            return BatteryState::FULL;
        }
    }

    // Calculate average voltage from first half and second half of history
    int half_count = state::history_count / 2;
    float first_half_sum = 0;
    float second_half_sum = 0;

    // First half (older samples)
    for (int i = 0; i < half_count; i++) {
        int idx = (state::history_index + config::HISTORY_SIZE - state::history_count + i) % config::HISTORY_SIZE;
        first_half_sum += state::voltage_history[idx];
    }

    // Second half (newer samples)
    for (int i = half_count; i < state::history_count; i++) {
        int idx = (state::history_index + config::HISTORY_SIZE - state::history_count + i) % config::HISTORY_SIZE;
        second_half_sum += state::voltage_history[idx];
    }

    float first_half_avg = first_half_sum / half_count;
    float second_half_avg = second_half_sum / (state::history_count - half_count);
    float voltage_change = second_half_avg - first_half_avg;

    // Determine state based on voltage trend
    if (voltage_change > config::VOLTAGE_CHANGE_THRESHOLD) {
        return BatteryState::CHARGING;  // Voltage increasing
    } else if (voltage_change < -config::VOLTAGE_CHANGE_THRESHOLD) {
        return BatteryState::DISCHARGING;  // Voltage decreasing
    } else {
        return BatteryState::STABLE;  // Voltage relatively constant
    }
}

// =============================================================================
// INITIALIZATION
// =============================================================================

bool init() {
    if (state::initialized) {
        Serial.println("[BATTERY] Already initialized");
        return true;
    }

    // Configure ADC unit (ESP-IDF oneshot API)
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = BAT_ADC_UNIT;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &g_adc_handle));

    // Configure channel: 12-bit, 0-3.3V range (ADC_ATTEN_DB_12 = old ADC_11db)
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    chan_cfg.atten    = ADC_ATTEN_DB_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc_handle, BAT_ADC_CHANNEL, &chan_cfg));

    // Calibration (curve fitting on ESP32-S3)
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id  = BAT_ADC_UNIT;
    cali_cfg.chan     = BAT_ADC_CHANNEL;
    cali_cfg.atten    = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_cali_handle) != ESP_OK) {
        g_cali_handle = nullptr;  // Calibration unavailable, fall back to raw
    }

    state::initialized = true;
    Serial.println("[BATTERY] ✓ Battery monitoring initialized");
    Serial.printf("[BATTERY]   GPIO: %d | Divider: 1:%.1f | Range: %.1fV-%.1fV\n",
                  config::BAT_ADC_PIN, config::VOLTAGE_DIVIDER,
                  config::VBAT_MIN, config::VBAT_MAX);

    return true;
}

void update() {
    if (!state::initialized) {
        init();
    }

    // Get current voltage and update history
    float voltage = getVoltage();
    updateVoltageHistory(voltage);
}

// =============================================================================
// BASIC READING FUNCTIONS
// =============================================================================

uint16_t getRawADC() {
    if (!state::initialized) {
        init();
    }

    // Read multiple samples and average for stability
    uint32_t sum = 0;
    for (int i = 0; i < config::ADC_SAMPLES; i++) {
        int raw = 0;
        adc_oneshot_read(g_adc_handle, BAT_ADC_CHANNEL, &raw);
        sum += (uint32_t)raw;
        delayMicroseconds(100);
    }
    return (uint16_t)(sum / config::ADC_SAMPLES);
}

float getVoltage() {
    uint16_t adc_raw = getRawADC();

    float adc_voltage;
    if (g_cali_handle) {
        // Calibrated path (ESP32-S3 curve fitting) — returns millivolts
        int mv = 0;
        adc_cali_raw_to_voltage(g_cali_handle, adc_raw, &mv);
        adc_voltage = mv / 1000.0f;
    } else {
        // Uncalibrated fallback
        adc_voltage = (adc_raw / (float)config::ADC_RESOLUTION) * config::ADC_REFERENCE;
    }

    float raw_voltage = adc_voltage * config::VOLTAGE_DIVIDER;

    // Apply EMA smoothing to reduce percentage fluctuations
    if (!state::ema_initialized) {
        state::smoothed_voltage = raw_voltage;
        state::ema_initialized = true;
    } else {
        state::smoothed_voltage = config::EMA_ALPHA * raw_voltage +
                                   (1.0f - config::EMA_ALPHA) * state::smoothed_voltage;
    }

    return state::smoothed_voltage;
}

float getVoltageRaw() {
    uint16_t adc_raw = getRawADC();

    // Convert ADC value to voltage (after voltage divider)
    float adc_voltage = (adc_raw / (float)config::ADC_RESOLUTION) * config::ADC_REFERENCE;

    // Apply voltage divider compensation to get actual battery voltage
    return adc_voltage * config::VOLTAGE_DIVIDER;
}

int getPercent(float voltage) {
    // Clamp to valid range
    if (voltage >= config::VBAT_MAX) return 100;
    if (voltage <= config::VBAT_MIN) return 0;

    // Li-Ion discharge curve lookup table
    // Calibrated from real discharge test (2026-02-13, 7.5h runtime)
    // Multimeter readings mapped to time-based percentage
    // Format: {voltage, percentage}
    static const struct {
        float voltage;
        int percent;
    } discharge_curve[] = {
        {4.12, 100},  // Full charge (measured after USB disconnect)
        {4.04, 85},   // Measured data point
        {3.96, 78},   // Interpolated
        {3.88, 71},   // Measured data point
        {3.80, 63},   // Interpolated
        {3.74, 57},   // Interpolated
        {3.68, 52},   // Measured data point
        {3.62, 47},   // Interpolated
        {3.56, 42},   // Measured data point
        {3.53, 35},   // Measured data point
        {3.48, 27},   // Interpolated
        {3.40, 17},   // Measured data point
        {3.36, 10},   // Measured data point
        {3.26, 4},    // Measured data point
        {3.10, 1},    // Near death (display pulsing)
        {3.02, 0}     // Empty (system unstable)
    };

    static constexpr int curve_size = sizeof(discharge_curve) / sizeof(discharge_curve[0]);

    // Find the two points to interpolate between
    for (int i = 0; i < curve_size - 1; i++) {
        if (voltage >= discharge_curve[i + 1].voltage) {
            // Linear interpolation between two points
            float v_high = discharge_curve[i].voltage;
            float v_low = discharge_curve[i + 1].voltage;
            int p_high = discharge_curve[i].percent;
            int p_low = discharge_curve[i + 1].percent;

            float voltage_range = v_high - v_low;
            float percent_range = (float)(p_high - p_low);
            float voltage_offset = voltage - v_low;

            int percent = p_low + (int)((voltage_offset / voltage_range) * percent_range);

            // Ensure valid range
            return constrain(percent, 0, 100);
        }
    }

    // Voltage below minimum point in table
    return 0;
}

int getPercent() {
    float voltage = getVoltage();
    return getPercent(voltage);
}

// =============================================================================
// STATUS FUNCTIONS
// =============================================================================

BatteryState getBatteryState() {
    // Use cached EMA voltage — avoids redundant 15-sample ADC read on every call.
    // update() samples the ADC and populates smoothed_voltage; all callers use that.
    float voltage = state::ema_initialized ? state::smoothed_voltage : getVoltage();
    return detectBatteryState(voltage);
}

BatteryStatus getStatus() {
    BatteryStatus status;

    // Single ADC read for the whole status snapshot.
    // getBatteryState() and getPercent() reuse the cached smoothed_voltage.
    status.voltage = getVoltage();
    status.adc_raw = 0;  // Not sampled separately; use 'battery raw' serial cmd for diagnostics
    status.percent = getPercent(status.voltage);
    status.battery_state = detectBatteryState(status.voltage);

    // Percentage is unreliable (estimate) when charging
    status.percent_is_estimate = (status.battery_state == BatteryState::CHARGING);

    status.valid = (status.voltage >= config::VBAT_MIN - 0.5 &&
                   status.voltage <= config::VBAT_MAX + 0.5);

    return status;
}

bool isCharging() {
    BatteryState state = getBatteryState();
    return (state == BatteryState::CHARGING);
}

// =============================================================================
// WARNING FUNCTIONS
// =============================================================================

bool isLowBattery() {
    float voltage = state::ema_initialized ? state::smoothed_voltage : getVoltage();
    return (voltage < config::LOW_BATTERY_VOLTAGE &&
            detectBatteryState(voltage) == BatteryState::DISCHARGING);
}

bool isCriticalBattery() {
    float voltage = state::ema_initialized ? state::smoothed_voltage : getVoltage();
    return (voltage < config::CRITICAL_VOLTAGE &&
            detectBatteryState(voltage) == BatteryState::DISCHARGING);
}

void checkBatteryWarnings() {
    BatteryState state = getBatteryState();

    // Only check warnings when discharging (not while charging or stable)
    if (state != BatteryState::DISCHARGING) {
        return;
    }

    BatteryStatus status = getStatus();
    unsigned long now = millis();

    // Critical battery warning (< 10%)
    if (status.voltage < config::CRITICAL_VOLTAGE) {
        if (now - state::last_warning_time >= config::CRITICAL_WARNING_INTERVAL) {
            Serial.printf("[BATTERY] ⚠️  CRITICAL: %.2fV (%d%%) - Connect charger immediately!\n",
                          status.voltage, status.percent);
            state::last_warning_time = now;
        }
    }
    // Low battery warning (< 20%)
    else if (status.voltage < config::LOW_BATTERY_VOLTAGE) {
        if (now - state::last_warning_time >= config::LOW_BATTERY_WARNING_INTERVAL) {
            Serial.printf("[BATTERY] ⚠️  LOW: %.2fV (%d%%) - Charge soon\n",
                          status.voltage, status.percent);
            state::last_warning_time = now;
        }
    }

    // Emergency shutdown warning (< 3.0V)
    if (status.voltage < config::SHUTDOWN_VOLTAGE) {
        Serial.printf("[BATTERY] 🔴 EMERGENCY: %.2fV (%d%%) - Shutting down to protect battery!\n",
                      status.voltage, status.percent);
        // Note: Actual shutdown would require additional system-level integration
    }
}

// =============================================================================
// DIAGNOSTIC FUNCTIONS
// =============================================================================

void printHardwareInfo() {
    Serial.println("[BATTERY] Hardware Configuration:");
    Serial.printf("  GPIO Pin: %d\n", config::BAT_ADC_PIN);
    Serial.printf("  Voltage Divider: 1:%.1f (R5=200K, R9=100K)\n", config::VOLTAGE_DIVIDER);
    Serial.printf("  Voltage Range: %.1fV - %.1fV\n", config::VBAT_MIN, config::VBAT_MAX);
    Serial.printf("  ADC Resolution: %d-bit (0-%d)\n", 12, config::ADC_RESOLUTION);
    Serial.printf("  ADC Reference: %.1fV\n", config::ADC_REFERENCE);
    Serial.printf("  Samples per Reading: %d\n", config::ADC_SAMPLES);
}

void printStatus() {
    BatteryStatus status = getStatus();

    Serial.println("[BATTERY] Status:");
    Serial.printf("  Voltage: %.2fV (raw: %.2fV)\n", status.voltage, getVoltageRaw());
    Serial.printf("  Percentage: %d%%%s\n", status.percent,
                  status.percent_is_estimate ? " (estimate)" : "");
    Serial.printf("  Battery State: %s\n", batteryStateToString(status.battery_state));
    Serial.printf("  Raw ADC: %d / %d\n", status.adc_raw, config::ADC_RESOLUTION);
    Serial.printf("  Valid: %s\n", status.valid ? "Yes" : "No");
    Serial.printf("  History Samples: %d / %d\n", state::history_count, config::HISTORY_SIZE);
    Serial.printf("  EMA Alpha: %.2f\n", config::EMA_ALPHA);
}

void setPeriodicMonitoring(bool enable) {
    state::monitoring_enabled = enable;
    if (enable) {
        Serial.printf("[BATTERY] Periodic monitoring enabled (interval: %lu seconds)\n",
                      config::MONITORING_INTERVAL / 1000);
        state::last_monitoring_time = millis();  // Reset timer
    } else {
        Serial.println("[BATTERY] Periodic monitoring disabled");
    }
}

bool isMonitoringEnabled() {
    return state::monitoring_enabled;
}

void updatePeriodicMonitoring() {
    if (!state::monitoring_enabled) {
        return;
    }

    unsigned long now = millis();
    if (now - state::last_monitoring_time >= config::MONITORING_INTERVAL) {
        BatteryStatus status = getStatus();
        Serial.printf("[BATTERY] %.2fV | %s | %d%%%s\n",
                      status.voltage,
                      batteryStateToString(status.battery_state),
                      status.percent,
                      status.percent_is_estimate ? " (estimate)" : "");

        state::last_monitoring_time = now;
    }
}

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

const char* batteryStateToString(BatteryState state) {
    switch (state) {
        case BatteryState::CHARGING:     return "Charging";
        case BatteryState::DISCHARGING:  return "Discharging";
        case BatteryState::FULL:         return "Full";
        case BatteryState::STABLE:       return "Stable";
        case BatteryState::UNKNOWN:      return "Unknown";
        default:                         return "Invalid";
    }
}

} // namespace battery
