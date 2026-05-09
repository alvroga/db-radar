#ifndef STANDBY_MANAGER_H
#define STANDBY_MANAGER_H

#include "core/arduino_compat.h"

/**
 * @file standby_manager.h
 * @brief Standby Mode Management for GPS Radar
 *
 * Manages low-power standby state with minimal power consumption while
 * keeping GPS active for continuous tracking.
 *
 * User Experience:
 * - Hold GPIO0 for 4 seconds → Enter standby
 * - Press GPIO0 → Wake from standby
 * - Display OFF, GPS ON, WiFi OFF
 *
 * Power Savings:
 * - Active mode: ~520mA
 * - Standby mode: ~55mA (89% reduction)
 */

namespace standby_manager {

// =============================================================================
// TYPES
// =============================================================================

/**
 * @brief Standby system states
 */
enum class StandbyState {
    ACTIVE,       ///< Normal operation
    ENTERING,     ///< Showing standby screen (2s transition)
    STANDBY,      ///< Low-power standby mode
    WAKING        ///< Waking from standby
};

/**
 * @brief Standby statistics
 */
struct StandbyStats {
    uint32_t total_standby_count;      ///< Total times standby was entered
    uint32_t total_standby_time_ms;    ///< Total time spent in standby
    uint32_t last_standby_duration_ms; ///< Duration of last standby session
};

// =============================================================================
// PUBLIC API
// =============================================================================

/**
 * @brief Initialize standby manager
 * @return true if successful, false on failure
 */
bool init();

/**
 * @brief Enter standby mode
 *
 * Sequence:
 * 1. Display standby screen for 2 seconds
 * 2. Fade backlight to 0%
 * 3. Disable WiFi/BLE
 * 4. Reduce task update rates
 * 5. Mark state as STANDBY
 */
void enterStandby();

/**
 * @brief Wake from standby mode
 *
 * Sequence:
 * 1. Restore backlight to saved brightness
 * 2. Resume LVGL timer
 * 3. Restore WiFi/BLE if previously enabled
 * 4. Return to radar screen
 * 5. Mark state as ACTIVE
 */
void wakeFromStandby();

/**
 * @brief Check if currently in standby
 * @return true if in STANDBY state, false otherwise
 */
bool isStandby();

/**
 * @brief Get current standby state
 * @return Current StandbyState
 */
StandbyState getState();

/**
 * @brief Get standby statistics
 * @return StandbyStats struct
 */
StandbyStats getStats();

/**
 * @brief Notify that user interaction occurred (touch or button press)
 * Resets the inactivity timer. Call on every button press and valid touch event.
 */
void notifyUserActivity();

/**
 * @brief Check if inactivity timeout has elapsed and queue auto-sleep if so.
 * Call from System Task every 1 second. No-op if auto_sleep is disabled or already in standby.
 */
void checkInactivityTimeout();

/**
 * @brief Convert StandbyState to string for logging
 * @param state StandbyState to convert
 * @return String representation
 */
const char* stateToString(StandbyState state);

} // namespace standby_manager

#endif // STANDBY_MANAGER_H
