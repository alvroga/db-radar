#include "i2c_manager.h"
#include "core/arduino_compat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace i2c_manager {

static const char* TAG = "I2C";
static i2c_master_bus_handle_t g_bus_handle = nullptr;
// Recursive mutex — serializes all bus transactions across tasks.
// Recursive because reinit() holds the mutex while calling init() which calls ping().
// ESP-IDF's internal I2C bus lock is not sufficient: concurrent calls from different
// tasks (UI Task polling CST820 every 10ms, System Task reading compass every 100ms)
// can leave the hardware FSM in ESP_ERR_INVALID_STATE despite the API being "thread-safe".
static SemaphoreHandle_t g_bus_mutex = nullptr;
static Stats g_stats;

// Device handle definitions — _handle is nullptr until init() registers them
DeviceHandle IMU_DEVICE_LOW  = {0x6A, "IMU_LOW",  nullptr};
DeviceHandle IMU_DEVICE_HIGH = {0x6B, "IMU_HIGH", nullptr};
DeviceHandle RTC_DEVICE      = {0x51, "RTC",      nullptr};
DeviceHandle TOUCH_DEVICE    = {0x15, "TOUCH",    nullptr};
DeviceHandle EXIO_DEVICE     = {0x20, "EXIO",     nullptr};
DeviceHandle COMPASS_DEVICE  = {0x0D, "COMPASS",  nullptr};

// ============================================================================
// Internal: register a device on the bus at init time
// ============================================================================
static bool registerDevice(DeviceHandle& dev, uint32_t scl_speed) {
    if (g_bus_handle == nullptr) return false;

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = dev.addr;
    cfg.scl_speed_hz    = scl_speed;

    esp_err_t err = i2c_master_bus_add_device(g_bus_handle, &cfg, &dev._handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register %s (0x%02X): %s",
                 dev.name, dev.addr, esp_err_to_name(err));
        dev._handle = nullptr;
        return false;
    }
    return true;
}

// ============================================================================
// init
// ============================================================================
bool init(const Config& config) {
    // Create mutex once — never destroyed (survives reinit cycles).
    if (g_bus_mutex == nullptr) {
        g_bus_mutex = xSemaphoreCreateRecursiveMutex();
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = (gpio_num_t)config.sda_pin;
    bus_cfg.scl_io_num        = (gpio_num_t)config.scl_pin;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_bus_handle);
    if (err != ESP_OK) {
        Serial.printf("[I2C] Bus init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    g_stats = Stats{};

    // Register all devices on the shared bus
    registerDevice(IMU_DEVICE_LOW,  config.frequency);
    registerDevice(IMU_DEVICE_HIGH, config.frequency);
    registerDevice(RTC_DEVICE,      config.frequency);
    registerDevice(TOUCH_DEVICE,    config.frequency);
    registerDevice(EXIO_DEVICE,     config.frequency);
    registerDevice(COMPASS_DEVICE,  config.frequency);

    Serial.printf("[I2C] Initialized: SDA=%d, SCL=%d, freq=%luHz\n",
                  config.sda_pin, config.scl_pin, (unsigned long)config.frequency);

    // Basic connectivity check
    if (!ping(EXIO_DEVICE)) {
        Serial.println("[I2C] WARNING: EXIO device not responding after init");
    }

    return true;
}

// ============================================================================
// read — register write then read (combined transaction)
// Serialized by g_bus_mutex — safe from any task concurrently
// ============================================================================
bool read(DeviceHandle& dev, uint8_t reg, uint8_t* data, size_t len, int retries) {
    if (dev._handle == nullptr) return false;

    g_stats.total_ops++;

    // Serialize all bus access — prevents UI Task (touch) colliding with System Task (compass)
    if (g_bus_mutex == nullptr ||
        xSemaphoreTakeRecursive(g_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        g_stats.failed_ops++;
        return false;
    }

    bool success = false;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) {
            g_stats.retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        esp_err_t err = i2c_master_transmit_receive(
            dev._handle,
            &reg, 1,          // write: register address
            data, len,        // read: response data
            pdMS_TO_TICKS(50) // timeout
        );

        if (err == ESP_OK) {
            success = true;
            break;
        }

        if (attempt == retries) {
            Serial.printf("[I2C] %s reg=0x%02X read failed: %s\n",
                          dev.name, reg, esp_err_to_name(err));
        }
    }

    xSemaphoreGiveRecursive(g_bus_mutex);
    if (!success) g_stats.failed_ops++;
    return success;
}

// ============================================================================
// write — register address + data in single transaction
// ============================================================================
bool write(DeviceHandle& dev, uint8_t reg, const uint8_t* data, size_t len, int retries) {
    if (dev._handle == nullptr) return false;

    // Build combined buffer: [reg, data...]
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(buf + 1, data, len);

    g_stats.total_ops++;

    if (g_bus_mutex == nullptr ||
        xSemaphoreTakeRecursive(g_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        g_stats.failed_ops++;
        return false;
    }

    bool success = false;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) {
            g_stats.retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        esp_err_t err = i2c_master_transmit(
            dev._handle,
            buf, len + 1,
            pdMS_TO_TICKS(50)
        );

        if (err == ESP_OK) {
            success = true;
            break;
        }

        if (attempt == retries) {
            Serial.printf("[I2C] %s reg=0x%02X write failed: %s\n",
                          dev.name, reg, esp_err_to_name(err));
        }
    }

    xSemaphoreGiveRecursive(g_bus_mutex);
    if (!success) g_stats.failed_ops++;
    return success;
}

bool readByte(DeviceHandle& dev, uint8_t reg, uint8_t& value, int retries) {
    return read(dev, reg, &value, 1, retries);
}

bool writeByte(DeviceHandle& dev, uint8_t reg, uint8_t value, int retries) {
    return write(dev, reg, &value, 1, retries);
}

bool ping(DeviceHandle& dev) {
    if (dev._handle == nullptr || g_bus_handle == nullptr) return false;
    if (g_bus_mutex == nullptr ||
        xSemaphoreTakeRecursive(g_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool ok = i2c_master_probe(g_bus_handle, dev.addr, pdMS_TO_TICKS(10)) == ESP_OK;
    xSemaphoreGiveRecursive(g_bus_mutex);
    return ok;
}

bool resetBus() {
    if (g_bus_handle == nullptr) return false;
    esp_err_t err = i2c_master_bus_reset(g_bus_handle);
    if (err == ESP_OK) {
        Serial.println("[I2C] Bus reset OK");
        return true;
    }
    Serial.printf("[I2C] Bus reset failed: %s\n", esp_err_to_name(err));
    return false;
}

bool reinit(const Config& config) {
    Serial.println("[I2C] Full re-initialization...");

    // Acquire bus mutex — blocks any in-flight read()/write() from other tasks
    // until reinit completes. Mutex is recursive so init() → ping() inside here
    // can re-acquire without deadlocking.
    if (g_bus_mutex != nullptr) {
        xSemaphoreTakeRecursive(g_bus_mutex, portMAX_DELAY);
    }

    // Clock recovery: send 9 SCL pulses to release any peripheral holding SDA low.
    // Must happen BEFORE deleting the bus handle — i2c_master_bus_reset() requires
    // a valid handle. This fixes TCA9554/QMC5883L stuck mid-transaction after long
    // standby (ESP_ERR_TIMEOUT on wake that a plain driver restart cannot clear).
    if (g_bus_handle != nullptr) {
        esp_err_t rst = i2c_master_bus_reset(g_bus_handle);
        Serial.printf("[I2C] Clock recovery (9 SCL pulses): %s\n", esp_err_to_name(rst));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Remove all registered device handles before deleting the bus
    DeviceHandle* devices[] = {
        &IMU_DEVICE_LOW, &IMU_DEVICE_HIGH, &RTC_DEVICE,
        &TOUCH_DEVICE, &EXIO_DEVICE, &COMPASS_DEVICE
    };
    for (auto* dev : devices) {
        if (dev->_handle != nullptr) {
            i2c_master_bus_rm_device(dev->_handle);
            dev->_handle = nullptr;
        }
    }

    if (g_bus_handle != nullptr) {
        i2c_del_master_bus(g_bus_handle);
        g_bus_handle = nullptr;
    }

    vTaskDelay(pdMS_TO_TICKS(20));  // Let bus lines settle before reinit

    bool ok = init(config);  // init() is recursive-safe: re-acquires mutex for ping()
    if (ok) {
        Serial.println("[I2C] Re-initialization complete");
    } else {
        Serial.println("[I2C] Re-initialization FAILED");
    }

    if (g_bus_mutex != nullptr) {
        xSemaphoreGiveRecursive(g_bus_mutex);
    }
    return ok;
}

void scanBus() {
    if (g_bus_handle == nullptr) {
        Serial.println("[I2C] Bus not initialized");
        return;
    }

    struct KnownDevice { uint8_t addr; const char* name; };
    static const KnownDevice known[] = {
        {0x0D, "QMC5883L (Compass)"},
        {0x15, "CST820 (Touch)"},
        {0x1E, "HMC5883L (Compass)"},
        {0x20, "TCA9554 (IO Expander)"},
        {0x51, "PCF85063 (RTC)"},
        {0x6A, "QMI8658 (IMU low)"},
        {0x6B, "QMI8658 (IMU high)"},
    };

    // Hold mutex for full scan — prevents touch/compass reads during diagnostic output
    if (g_bus_mutex != nullptr) {
        xSemaphoreTakeRecursive(g_bus_mutex, portMAX_DELAY);
    }

    Serial.println("==== I2C Bus Scan ====");
    int found = 0;
    for (uint8_t addr = 1; addr < 0x7F; addr++) {
        if (i2c_master_probe(g_bus_handle, addr, pdMS_TO_TICKS(5)) == ESP_OK) {
            found++;
            const char* name = "Unknown";
            for (const auto& d : known) {
                if (d.addr == addr) { name = d.name; break; }
            }
            Serial.printf("  0x%02X  -  %s\n", addr, name);
        }
    }

    if (found == 0) {
        Serial.println("  No devices found! Check wiring.");
    } else {
        Serial.printf("Found %d device(s)\n", found);
    }
    Serial.println("======================");

    if (g_bus_mutex != nullptr) {
        xSemaphoreGiveRecursive(g_bus_mutex);
    }
}

const Stats& getStats() { return g_stats; }

// ============================================================================
// TCA9554 IO Expander
// ============================================================================
namespace exio {

bool begin(State& state) {
    Serial.println("[EXIO] Starting initialization...");

    if (!writeByte(EXIO_DEVICE, REG_CONFIG, 0x00)) {
        Serial.println("[EXIO] Failed to configure as outputs");
        return false;
    }

    uint8_t current = 0xFF;
    if (readByte(EXIO_DEVICE, REG_OUTPUT, current)) {
        state.out = current;
        Serial.printf("[EXIO] Read current state: 0x%02X\n", current);
    } else {
        Serial.println("[EXIO] Failed to read output state, using default");
        state.out = 0xFF;
    }

    Serial.printf("[EXIO] Initialized, state: 0x%02X\n", state.out);
    return true;
}

bool set(Pin pin, bool high, State& state) {
    if (pin > 7) return false;

    if (high) {
        state.out |= (1u << pin);
    } else {
        state.out &= ~(1u << pin);
    }

    return writeByte(EXIO_DEVICE, REG_OUTPUT, state.out);
}

bool writeOutput(const State& state) {
    return writeByte(EXIO_DEVICE, REG_OUTPUT, state.out);
}

bool readInput(uint8_t& value) {
    return readByte(EXIO_DEVICE, REG_INPUT, value);
}

bool readOutput(uint8_t& value) {
    return readByte(EXIO_DEVICE, REG_OUTPUT, value);
}

bool rawWrite(uint8_t reg, uint8_t value) {
    return writeByte(EXIO_DEVICE, reg, value);
}

bool rawRead(uint8_t reg, uint8_t& value) {
    return readByte(EXIO_DEVICE, reg, value);
}

} // namespace exio
} // namespace i2c_manager
