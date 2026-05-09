#ifndef BATTERY_H
#define BATTERY_H

#include "core/arduino_compat.h"

/**
 * @file battery.h
 * @brief Battery monitoring system for Waveshare ESP32-S3-Touch-LCD-2.1
 *
 * Hardware:
 * - GPIO4 connected to BAT_ADC via 1:3 voltage divider (R5=200K, R9=100K)
 * - ETA6098 charging IC with LED1 indicator
 * - MX1.25 2-pin battery connector for 3.7V Li-Ion/LiPo batteries
 * - Battery voltage range: 3.0V (empty) to 4.2V (full)
 * - ADC measurement range: 1.0V to 1.4V (after voltage divider)
 *
 * Features:
 * - Real-time battery voltage monitoring
 * - Battery percentage calculation (0-100%)
 * - Charging state detection via voltage trend analysis
 * - Low battery warnings (only when discharging)
 * - Serial diagnostic commands
 */

namespace battery {

// =============================================================================
// ENUMERATIONS
// =============================================================================

/**
 * @brief Battery state detection via voltage trend analysis
 *
 * Instead of trying to detect USB vs Battery (unreliable at mid-charge levels),
 * we track voltage over time to determine actual battery state.
 */
enum class BatteryState {
    CHARGING,       // Voltage increasing over time (charge current flowing)
    DISCHARGING,    // Voltage decreasing over time (device consuming power)
    FULL,           // At ~4.20V and stable (100% charged)
    STABLE,         // Voltage stable but not full (idle/resting)
    UNKNOWN         // Not enough data yet to determine trend
};

// =============================================================================
// STRUCTURES
// =============================================================================

/**
 * @brief Complete battery status information
 */
struct BatteryStatus {
    float voltage;              // Battery voltage in volts (V)
    int percent;                // Battery percentage (0-100%)
    BatteryState battery_state; // Current battery state (charging/discharging/full/stable)
    bool percent_is_estimate;   // True when percentage is unreliable (e.g., while charging)
    uint16_t adc_raw;          // Raw ADC reading (0-4095)
    bool valid;                 // True if reading is valid
};

// =============================================================================
// INITIALIZATION
// =============================================================================

/**
 * @brief Initialize battery monitoring system
 *
 * Sets up ADC on GPIO4 with proper configuration for battery voltage reading.
 * Must be called once during system initialization.
 *
 * @return true if initialization successful, false otherwise
 */
bool init();

/**
 * @brief Update battery monitoring (call regularly from main loop)
 *
 * This function should be called periodically (every loop iteration or every second)
 * to collect voltage samples for trend analysis. It's lightweight and only updates
 * the voltage history buffer at configured intervals (every 30 seconds).
 *
 * Call this from your main loop or FreeRTOS task to enable automatic battery
 * state detection.
 */
void update();

// =============================================================================
// BASIC READING FUNCTIONS
// =============================================================================

/**
 * @brief Read current battery voltage (EMA smoothed)
 *
 * Reads ADC on GPIO4, applies voltage divider compensation (×3),
 * and returns EMA-smoothed battery voltage for stable display.
 *
 * @return Smoothed battery voltage in volts (V), or -1.0 if error
 */
float getVoltage();

/**
 * @brief Read raw (unsmoothed) battery voltage
 *
 * Returns the instantaneous voltage reading without EMA smoothing.
 * Useful for diagnostics and calibration.
 *
 * @return Raw battery voltage in volts (V)
 */
float getVoltageRaw();

/**
 * @brief Get battery percentage
 *
 * Calculates battery percentage based on Li-Ion voltage curve.
 * Linear approximation between 3.0V (0%) and 4.2V (100%).
 *
 * @return Battery percentage (0-100%), or -1 if error
 */
int getPercent();

/**
 * @brief Get battery percentage from specific voltage
 *
 * @param voltage Battery voltage in volts
 * @return Battery percentage (0-100%)
 */
int getPercent(float voltage);

/**
 * @brief Read raw ADC value
 *
 * @return Raw 12-bit ADC value (0-4095), or 0 if error
 */
uint16_t getRawADC();

// =============================================================================
// STATUS FUNCTIONS
// =============================================================================

/**
 * @brief Get complete battery status
 *
 * Returns comprehensive battery information including voltage,
 * percentage, and battery state (charging/discharging/full).
 *
 * @return BatteryStatus structure with all battery information
 */
BatteryStatus getStatus();

/**
 * @brief Detect current battery state via voltage trend analysis
 *
 * Analyzes voltage history over time to determine battery state:
 * - CHARGING: Voltage increasing (charge current flowing)
 * - DISCHARGING: Voltage decreasing (device consuming power)
 * - FULL: At ~4.20V and stable (100% charged)
 * - STABLE: Voltage stable but not full
 * - UNKNOWN: Not enough data yet
 *
 * @return BatteryState enum
 */
BatteryState getBatteryState();

/**
 * @brief Check if battery is charging
 *
 * @return true if battery state is CHARGING
 */
bool isCharging();

// =============================================================================
// WARNING FUNCTIONS
// =============================================================================

/**
 * @brief Check battery warnings
 *
 * Monitors battery level and generates serial warnings at:
 * - Low battery (< 3.5V / 20%): Warning every 5 minutes
 * - Critical battery (< 3.3V / 10%): Warning every 1 minute
 *
 * Call this periodically (e.g., every 30 seconds) from main loop or task.
 */
void checkBatteryWarnings();

/**
 * @brief Check if battery is low
 *
 * @return true if battery < 20% (< 3.5V)
 */
bool isLowBattery();

/**
 * @brief Check if battery is critical
 *
 * @return true if battery < 10% (< 3.3V)
 */
bool isCriticalBattery();

// =============================================================================
// DIAGNOSTIC FUNCTIONS
// =============================================================================

/**
 * @brief Print hardware configuration info
 *
 * Outputs battery monitoring hardware details to serial:
 * - GPIO pin assignment
 * - Voltage divider ratio
 * - Voltage range
 * - ADC resolution
 */
void printHardwareInfo();

/**
 * @brief Print current battery status
 *
 * Outputs formatted battery status to serial with all information.
 */
void printStatus();

/**
 * @brief Enable/disable periodic battery monitoring
 *
 * When enabled, battery status is logged to serial at regular intervals.
 *
 * @param enable true to enable monitoring, false to disable
 */
void setPeriodicMonitoring(bool enable);

/**
 * @brief Check if periodic monitoring is enabled
 *
 * @return true if periodic monitoring is active
 */
bool isMonitoringEnabled();

/**
 * @brief Update periodic monitoring (call from main loop)
 *
 * If periodic monitoring is enabled, this function will print
 * battery status at configured intervals.
 */
void updatePeriodicMonitoring();

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

/**
 * @brief Convert BatteryState enum to string
 *
 * @param state BatteryState enum value
 * @return String representation ("Charging", "Discharging", "Full", "Stable", "Unknown")
 */
const char* batteryStateToString(BatteryState state);

} // namespace battery

#endif // BATTERY_H
