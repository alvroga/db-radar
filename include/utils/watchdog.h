#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "core/arduino_compat.h"
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * @file watchdog.h
 * @brief ESP32 Task Watchdog Timer (TWDT) wrapper for stability monitoring
 *
 * Provides hardware-level detection of hung tasks. Each task subscribes
 * to the watchdog and must call feed() periodically to prevent reset.
 *
 * Features:
 * - 30-second timeout (configurable)
 * - Logs warning instead of panic by default
 * - Per-task subscription
 * - Statistics tracking
 */

namespace watchdog {

// =============================================================================
// CONFIGURATION
// =============================================================================

struct Config {
    uint32_t timeout_seconds = 30;    // Watchdog timeout (default: 30s)
    bool panic_on_timeout = false;    // false = log warning only, true = reset system
    bool enable_idle_task = false;    // Watch idle tasks (usually not needed)
};

// =============================================================================
// WATCHDOG STATISTICS
// =============================================================================

struct Stats {
    uint32_t total_feeds;             // Total feed calls
    uint32_t timeout_warnings;        // Number of timeout warnings
    uint32_t subscribed_tasks;        // Number of subscribed tasks
    bool initialized;                 // Watchdog initialized
    uint32_t last_feed_ms;            // Last feed timestamp
};

// =============================================================================
// PUBLIC API
// =============================================================================

/**
 * @brief Initialize the Task Watchdog Timer
 * @param config Watchdog configuration
 * @return true if successful, false on failure
 *
 * Should be called early in main.cpp, before task creation.
 */
bool init(const Config& config = Config{});

/**
 * @brief Deinitialize the watchdog (for cleanup)
 */
void deinit();

/**
 * @brief Subscribe current task to the watchdog
 * @return true if successful, false on failure
 *
 * Call from within each task's initialization.
 * The task must call feed() periodically to prevent timeout.
 */
bool subscribe();

/**
 * @brief Subscribe a specific task to the watchdog
 * @param task_handle FreeRTOS task handle
 * @return true if successful, false on failure
 */
bool subscribeTask(TaskHandle_t task_handle);

/**
 * @brief Unsubscribe current task from the watchdog
 * @return true if successful, false on failure
 */
bool unsubscribe();

/**
 * @brief Unsubscribe a specific task from the watchdog
 * @param task_handle FreeRTOS task handle
 * @return true if successful, false on failure
 */
bool unsubscribeTask(TaskHandle_t task_handle);

/**
 * @brief Feed the watchdog (reset timeout counter)
 * @return true if successful, false on failure
 *
 * Call this from each subscribed task's main loop.
 * Should be called at least once per timeout period.
 */
bool feed();

/**
 * @brief Check if watchdog is initialized
 * @return true if initialized
 */
bool isInitialized();

/**
 * @brief Get watchdog statistics
 * @return Stats structure with watchdog metrics
 */
Stats getStats();

/**
 * @brief Print watchdog status to serial
 */
void printStatus();

/**
 * @brief Manually trigger a watchdog check (for testing)
 * @return true if all subscribed tasks are responsive
 */
bool checkTasks();

} // namespace watchdog

#endif // WATCHDOG_H
