#include "utils/watchdog.h"
#include <esp_task_wdt.h>
#include <esp_idf_version.h>

namespace watchdog {

// =============================================================================
// INTERNAL STATE
// =============================================================================

static Config g_config;
static Stats g_stats = {0, 0, 0, false, 0};
static bool g_initialized = false;

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bool init(const Config& config) {
    if (g_initialized) {
        Serial.println("[WATCHDOG] Already initialized");
        return true;
    }

    g_config = config;

    Serial.printf("[WATCHDOG] Initializing TWDT: timeout=%lus, panic=%s\n",
                  config.timeout_seconds,
                  config.panic_on_timeout ? "yes" : "no");

    // ESP-IDF API differs between versions
    // Older ESP-IDF (< 5.0) uses esp_task_wdt_init(timeout_ms, panic_on_timeout)
    // Newer ESP-IDF (>= 5.0) uses esp_task_wdt_init(&config)

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP-IDF 5.0+ API
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = config.timeout_seconds * 1000,
        .idle_core_mask = (uint32_t)(config.enable_idle_task ? 0x03 : 0x00),
        .trigger_panic = config.panic_on_timeout
    };

    esp_err_t err = esp_task_wdt_init(&twdt_config);
    if (err == ESP_ERR_INVALID_STATE) {
        Serial.println("[WATCHDOG] TWDT already initialized, reconfiguring...");
        err = esp_task_wdt_reconfigure(&twdt_config);
    }
#else
    // ESP-IDF 4.x API (used by Arduino-ESP32 2.x)
    esp_err_t err = esp_task_wdt_init(config.timeout_seconds, config.panic_on_timeout);
    if (err == ESP_ERR_INVALID_STATE) {
        // TWDT already initialized - this is OK, we can still add tasks
        Serial.println("[WATCHDOG] TWDT already initialized by system");
        err = ESP_OK;
    }
#endif

    if (err != ESP_OK) {
        Serial.printf("[WATCHDOG] Failed to initialize TWDT: %s\n", esp_err_to_name(err));
        return false;
    }

    g_initialized = true;
    g_stats.initialized = true;

    Serial.println("[WATCHDOG] TWDT initialized successfully");
    return true;
}

void deinit() {
    if (!g_initialized) {
        return;
    }

    esp_err_t err = esp_task_wdt_deinit();
    if (err != ESP_OK) {
        Serial.printf("[WATCHDOG] Failed to deinit TWDT: %s\n", esp_err_to_name(err));
        return;
    }

    g_initialized = false;
    g_stats.initialized = false;
    g_stats.subscribed_tasks = 0;

    Serial.println("[WATCHDOG] TWDT deinitialized");
}

bool subscribe() {
    if (!g_initialized) {
        Serial.println("[WATCHDOG] Cannot subscribe - not initialized");
        return false;
    }

    esp_err_t err = esp_task_wdt_add(nullptr);  // nullptr = current task
    if (err == ESP_ERR_INVALID_ARG) {
        // Task already subscribed
        return true;
    }

    if (err != ESP_OK) {
        Serial.printf("[WATCHDOG] Failed to subscribe task: %s\n", esp_err_to_name(err));
        return false;
    }

    g_stats.subscribed_tasks++;
    Serial.printf("[WATCHDOG] Task subscribed (total: %lu)\n", g_stats.subscribed_tasks);
    return true;
}

bool subscribeTask(TaskHandle_t task_handle) {
    if (!g_initialized) {
        Serial.println("[WATCHDOG] Cannot subscribe - not initialized");
        return false;
    }

    if (!task_handle) {
        Serial.println("[WATCHDOG] Invalid task handle");
        return false;
    }

    esp_err_t err = esp_task_wdt_add(task_handle);
    if (err == ESP_ERR_INVALID_ARG) {
        // Task already subscribed
        return true;
    }

    if (err != ESP_OK) {
        Serial.printf("[WATCHDOG] Failed to subscribe task: %s\n", esp_err_to_name(err));
        return false;
    }

    g_stats.subscribed_tasks++;
    return true;
}

bool unsubscribe() {
    if (!g_initialized) {
        return false;
    }

    esp_err_t err = esp_task_wdt_delete(nullptr);  // nullptr = current task
    if (err != ESP_OK) {
        Serial.printf("[WATCHDOG] Failed to unsubscribe task: %s\n", esp_err_to_name(err));
        return false;
    }

    if (g_stats.subscribed_tasks > 0) {
        g_stats.subscribed_tasks--;
    }
    return true;
}

bool unsubscribeTask(TaskHandle_t task_handle) {
    if (!g_initialized || !task_handle) {
        return false;
    }

    esp_err_t err = esp_task_wdt_delete(task_handle);
    if (err != ESP_OK) {
        Serial.printf("[WATCHDOG] Failed to unsubscribe task: %s\n", esp_err_to_name(err));
        return false;
    }

    if (g_stats.subscribed_tasks > 0) {
        g_stats.subscribed_tasks--;
    }
    return true;
}

bool feed() {
    if (!g_initialized) {
        return false;
    }

    esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK) {
        // Task might not be subscribed
        return false;
    }

    g_stats.total_feeds++;
    g_stats.last_feed_ms = millis();
    return true;
}

bool isInitialized() {
    return g_initialized;
}

Stats getStats() {
    return g_stats;
}

void printStatus() {
    Serial.println("==== Watchdog Status ====");
    Serial.printf("Initialized: %s\n", g_initialized ? "YES" : "NO");
    Serial.printf("Timeout: %lu seconds\n", g_config.timeout_seconds);
    Serial.printf("Panic on timeout: %s\n", g_config.panic_on_timeout ? "YES" : "NO");
    Serial.printf("Subscribed tasks: %lu\n", g_stats.subscribed_tasks);
    Serial.printf("Total feeds: %lu\n", g_stats.total_feeds);
    Serial.printf("Timeout warnings: %lu\n", g_stats.timeout_warnings);
    Serial.printf("Last feed: %lu ms ago\n", millis() - g_stats.last_feed_ms);
    Serial.println("=========================");
}

bool checkTasks() {
    if (!g_initialized) {
        return false;
    }

    // The TWDT automatically checks tasks
    // This function is mainly for manual status checking
    esp_task_wdt_status(nullptr);  // Check current task status
    return true;
}

} // namespace watchdog
