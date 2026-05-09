#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "core/arduino_compat.h"

/**
 * @file task_manager.h
 * @brief FreeRTOS Task Management for ESP32-S3 Touch LCD Template
 *
 * Implements advanced multitasking architecture to eliminate freezes
 * and improve system responsiveness. Addresses Priority 3.7 requirements.
 */

namespace task_manager {

// =============================================================================
// TASK CONFIGURATION
// =============================================================================

struct TaskConfig {
    // Task stack sizes (in bytes) - increased for safety
    static constexpr size_t UI_STACK_SIZE = 16384;     // UI + LVGL processing (doubled)
    static constexpr size_t I2C_STACK_SIZE = 8192;     // I2C operations (doubled)
    static constexpr size_t NETWORK_STACK_SIZE = 12288; // WiFi/BLE scanning (doubled)
    static constexpr size_t SYSTEM_STACK_SIZE = 8192;  // Memory, diagnostics (doubled)

    // Task priorities (higher number = higher priority)
    static constexpr UBaseType_t UI_PRIORITY = 5;      // Above FreeRTOS timer svc (1), below WiFi driver (22-23)
    static constexpr UBaseType_t I2C_PRIORITY = 2;     // Medium for device comm
    static constexpr UBaseType_t NETWORK_PRIORITY = 1; // Low for scanning
    static constexpr UBaseType_t SYSTEM_PRIORITY = 1;  // Low - GPS runs on HW UART (doesn't need high priority)

    // Core assignments
    static constexpr BaseType_t UI_CORE = 1;           // Core 1 for UI
    static constexpr BaseType_t OTHER_CORE = 0;        // Core 0 for everything else

    // Update intervals (in milliseconds)
    static constexpr uint32_t UI_UPDATE_MS = 10;       // 100 FPS max
    static constexpr uint32_t I2C_PROCESS_MS = 20;     // 50 Hz I2C processing
    static constexpr uint32_t NETWORK_UPDATE_MS = 200; // 5 Hz for sonar rhythm
    static constexpr uint32_t SYSTEM_UPDATE_MS = 200;   // 5 Hz — System Task does more than compass; compass sub-timer handles actual read rate

    // Queue sizes
    static constexpr size_t I2C_QUEUE_SIZE = 16;       // I2C request queue
    static constexpr size_t UI_QUEUE_SIZE = 16;        // UI update queue (increased from 8 for stability)
};

// =============================================================================
// I2C REQUEST SYSTEM
// =============================================================================

enum class I2CDeviceType {
    RTC,
    EXIO,
    TOUCH
};

enum class I2COperation {
    READ,
    WRITE,
    PING,
    RTC_TIME_SET    // Set RTC time from GPS (one-shot sync)
};

struct I2CRequest {
    I2CDeviceType device;
    I2COperation operation;
    uint8_t device_addr;
    uint8_t reg_addr;
    uint8_t* data;
    size_t data_len;
    bool success;
    uint32_t timestamp;
    TaskHandle_t requester_task;

    // Callback for completion notification
    void (*callback)(const I2CRequest& req);
};

// =============================================================================
// UI UPDATE SYSTEM
// =============================================================================

enum class UIUpdateType {
    STATUS_LABEL,
    SENSOR_DATA,
    NETWORK_INFO,
    MEMORY_STATS,
    RADAR_REFRESH,
    ZOOM_CHANGE,           // Cycle zoom level forward (button single press)
    ZOOM_CHANGE_REVERSE,   // Cycle zoom level backward (button double press)
    SETTINGS_SCREEN,       // Open settings screen (button long press)
    ENTER_STANDBY,         // Enter standby mode (button extra-long press)
    WAKE_STANDBY,          // Wake from standby mode (any button press in standby)
    BATTERY_UPDATE,        // Update battery label (queued from System Task - fixes race condition!)
    LOAD_RADAR_SCREEN,     // Transition from loading screen to radar screen (queued from setup())
    LOAD_AP_SCREEN,        // Transition from loading screen to AP upload mode screen
    LOAD_WIFI_SCREEN,      // Transition from loading screen to WiFi STA mode screen
    COMPASS_UPDATE,        // Update heading from compass (queued from I2C Task)
    BEACON_DBM_UPDATE,     // Update beacon dBm label (DEV mode, 50m zoom only)
    DEV_MODE_CHANGE        // Show/hide DEV label when dev mode is toggled at runtime
};

struct UIUpdate {
    UIUpdateType type         = UIUpdateType::STATUS_LABEL;
    char data[64]             = {};
    uint32_t timestamp        = 0;

    // Battery update data (used when type == BATTERY_UPDATE)
    int8_t battery_percent    = -1;   // -1 = invalid, 0-100 = valid percentage
    float battery_voltage     = 0.0f; // Raw battery voltage for dev mode display
    bool daylight_mode        = false; // True = use black text for visibility

    // Compass update data (used when type == COMPASS_UPDATE)
    float compass_heading     = 0.0f; // Compass heading in degrees (0-360)
};

// =============================================================================
// TASK HEALTH MONITORING
// =============================================================================

struct TaskHealth {
    uint32_t last_loop_time_ms;        // Timestamp of last loop completion
    uint32_t unresponsive_count;       // Count of consecutive unresponsive checks
    uint32_t recovery_attempts;        // Number of recovery attempts made
    bool is_suspended;                 // Task currently suspended for recovery
    static constexpr uint32_t MAX_RECOVERY_ATTEMPTS = 3;
    static constexpr uint32_t UNRESPONSIVE_THRESHOLD_MS = 5000;  // 5 seconds
};

// =============================================================================
// TASK STATISTICS
// =============================================================================

struct TaskStats {
    uint32_t loop_count;
    uint32_t cpu_utilization;  // Percentage
    uint32_t stack_high_water; // Remaining stack space
    uint32_t last_runtime_ms;
    bool is_healthy;
    char status_message[32];

    // Enhanced health tracking
    TaskHealth health;
};

struct SystemStats {
    TaskStats ui_task;
    TaskStats i2c_task;
    TaskStats network_task;
    TaskStats system_task;

    uint32_t total_i2c_requests;
    uint32_t failed_i2c_requests;
    uint32_t ui_updates_queued;
    uint32_t memory_usage_bytes;

    bool tasks_running;
    uint32_t uptime_ms;
};

// =============================================================================
// PUBLIC API
// =============================================================================

/**
 * @brief Initialize task manager and create all tasks
 * @return true if successful, false on failure
 */
bool init();

/**
 * @brief Start all FreeRTOS tasks
 * @return true if successful, false on failure
 */
bool startTasks();

/**
 * @brief Stop all tasks and return to loop-based architecture
 */
void stopTasks();

/**
 * @brief Check if tasks are running
 * @return true if tasks are active, false if using loop mode
 */
bool isTaskModeActive();

/**
 * @brief Check if FreeRTOS tasks have stabilized and are healthy
 * @return true if all enabled tasks are running and healthy, false otherwise
 */
bool isSystemStable();

/**
 * @brief Queue an I2C request for processing
 * @param req I2C request structure
 * @return true if queued successfully, false if queue full
 */
bool queueI2CRequest(const I2CRequest& req);

/**
 * @brief Queue a UI update for processing
 * @param update UI update structure
 * @return true if queued successfully, false if queue full
 */
bool queueUIUpdate(const UIUpdate& update);

/**
 * @brief Get current system statistics
 * @return System statistics structure
 */
SystemStats getSystemStats();

/**
 * @brief Enable/disable specific tasks for debugging
 * @param ui_enabled Enable UI task
 * @param i2c_enabled Enable I2C task
 * @param network_enabled Enable Network task
 * @param system_enabled Enable System task
 */
void setTasksEnabled(bool ui_enabled, bool i2c_enabled, bool network_enabled, bool system_enabled);

/**
 * @brief Print task status to serial for debugging
 */
void printTaskStatus();

// =============================================================================
// TASK HANDLES (for external monitoring)
// =============================================================================

extern TaskHandle_t ui_task_handle;
extern TaskHandle_t i2c_task_handle;
extern TaskHandle_t network_task_handle;
extern TaskHandle_t system_task_handle;

// =============================================================================
// SYNCHRONIZATION PRIMITIVES
// =============================================================================

extern QueueHandle_t i2c_request_queue;
extern QueueHandle_t ui_update_queue;
extern SemaphoreHandle_t display_mutex;
extern SemaphoreHandle_t i2c_mutex;
extern SemaphoreHandle_t ui_state_mutex;

// =============================================================================
// THREAD-SAFE UI STATE ACCESSORS
// =============================================================================

/**
 * @brief Thread-safe zoom level getter
 * @return Current zoom level index
 */
int getCurrentZoomLevel();

/**
 * @brief Thread-safe zoom level setter (cycles forward)
 */
void cycleZoomForward();

/**
 * @brief Thread-safe zoom level setter (cycles backward)
 */
void cycleZoomBackward();

/**
 * @brief Execute a function while holding the display mutex
 * @param func Function to execute
 * @param timeout_ms Mutex timeout in milliseconds
 * @return true if function was executed, false if mutex timeout
 *
 * Usage:
 *   withDisplayMutex([]() {
 *       lv_label_set_text(label, "text");
 *   });
 */
bool withDisplayMutex(void (*func)(), uint32_t timeout_ms = 50);

} // namespace task_manager

#endif // TASK_MANAGER_H