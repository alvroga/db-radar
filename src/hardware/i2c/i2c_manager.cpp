#include "i2c_manager.h"
#include "core/arduino_compat.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils/system_logger.h"

namespace i2c_manager {

static const char* TAG = "I2C";
static i2c_master_bus_handle_t g_bus_handle = nullptr;
static Config g_config;  // saved by init() so recovery paths know which pins to inspect
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
// Per-device forensic stats — see DeviceStatSnapshot in the header for why
// this exists alongside the older cross-device Stats. Index order matches
// DeviceStatSnapshot's documented output order.
// ============================================================================
struct InternalDeviceStats {
    uint32_t ops = 0;
    uint32_t fails = 0;
    uint32_t consecutive_fails = 0;
    uint32_t max_latency_us = 0;
    uint32_t last_fail_ms = 0;
    bool     has_failed = false;
};
static InternalDeviceStats g_dev_stats[NUM_DEVICES];

static int deviceIndex(DeviceHandle& dev) {
    if (&dev == &IMU_DEVICE_LOW)  return 0;
    if (&dev == &IMU_DEVICE_HIGH) return 1;
    if (&dev == &RTC_DEVICE)      return 2;
    if (&dev == &TOUCH_DEVICE)    return 3;
    if (&dev == &EXIO_DEVICE)     return 4;
    if (&dev == &COMPASS_DEVICE)  return 5;
    return -1;
}

static void recordDeviceOp(DeviceHandle& dev, bool success, uint32_t latency_us) {
    int idx = deviceIndex(dev);
    if (idx < 0) return;
    InternalDeviceStats& s = g_dev_stats[idx];
    s.ops++;
    if (latency_us > s.max_latency_us) s.max_latency_us = latency_us;
    if (success) {
        s.consecutive_fails = 0;
    } else {
        s.fails++;
        s.consecutive_fails++;
        s.has_failed = true;
        s.last_fail_ms = millis();
    }
}

void getDeviceStats(DeviceStatSnapshot out[NUM_DEVICES]) {
    static DeviceHandle* order[NUM_DEVICES] = {
        &IMU_DEVICE_LOW, &IMU_DEVICE_HIGH, &RTC_DEVICE,
        &TOUCH_DEVICE, &EXIO_DEVICE, &COMPASS_DEVICE
    };
    uint32_t now = millis();
    for (int i = 0; i < NUM_DEVICES; i++) {
        const InternalDeviceStats& s = g_dev_stats[i];
        out[i].name = order[i]->name;
        out[i].addr = order[i]->addr;
        out[i].ops = s.ops;
        out[i].fails = s.fails;
        out[i].consecutive_fails = s.consecutive_fails;
        out[i].max_latency_us = s.max_latency_us;
        out[i].ms_since_last_fail = s.has_failed ? (now - s.last_fail_ms) : 0xFFFFFFFF;
    }
}

// Reads SDA/SCL levels directly off the GPIO input register — safe to call at
// any time, including while the I2C peripheral owns the pins via the GPIO
// matrix, because it never touches pin direction/mode. Added 2026-08-02 for
// the FT-06 freeze investigation: the next wedge should tell us which of
// three states we're in (SDA low = bit-bang-recoverable, SCL low = only a
// slave reset/power-cycle helps, both high = not electrical at all).
static void logLineLevels(const char* context) {
    int sda = gpio_get_level((gpio_num_t)g_config.sda_pin);
    int scl = gpio_get_level((gpio_num_t)g_config.scl_pin);
    Serial.printf("[I2C] %s: SDA=%d SCL=%d\n", context, sda, scl);
    system_logger::logf(system_logger::Level::INFO, "I2C", "%s: SDA=%d SCL=%d", context, sda, scl);
}

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
// bitBangClockRecovery — raw-GPIO bus recovery, done BEFORE the I2C peripheral
// claims the pins.
//
// i2c_master_bus_reset() (used by resetBus(), below) cannot be trusted to do
// this on ESP32-S3: its wait loop polls i2c_ll_master_is_bus_clear_done(),
// which the IDF LL layer stubs to `return false` unconditionally on this SoC
// (confirmed by reading the IDF source, see memory/i2c_bus_freeze_recovery.md).
// The call returns ESP_OK without ever driving a clock pulse — a silent no-op
// that only looks like recovery. This function does the real thing: standard
// I2C bus-clear procedure (toggle SCL up to 9 times while SDA is held low by a
// wedged slave, then issue a STOP), driven directly on the raw pins while
// nothing else owns them.
//
// Must run before i2c_new_master_bus() — once the peripheral configures these
// pins as its I2C engine, bit-banging them as plain GPIO is undefined. This is
// also why it lives in init() rather than resetBus()/reinit(): a reboot with
// an already-wedged bus never gets past device_manager's EXIO init otherwise
// (reproduced 2026-08-02 — a soft reboot alone hung solid with no more serial
// output at all, since the existing "recovery" never touched the wedge).
// ============================================================================
static void bitBangClockRecovery(gpio_num_t scl_pin, gpio_num_t sda_pin) {
    gpio_config_t in_conf = {};
    in_conf.mode = GPIO_MODE_INPUT;
    in_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    in_conf.pin_bit_mask = (1ULL << scl_pin) | (1ULL << sda_pin);
    gpio_config(&in_conf);
    esp_rom_delay_us(10);

    if (gpio_get_level(sda_pin) == 1) return;  // bus already free — nothing to do

    Serial.println("[I2C] SDA held low before bus init — bit-banging clock recovery");

    gpio_config_t od_conf = {};
    od_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    od_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    od_conf.pin_bit_mask = (1ULL << scl_pin) | (1ULL << sda_pin);
    gpio_config(&od_conf);

    gpio_set_level(sda_pin, 1);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(5);

    for (int i = 0; i < 9 && gpio_get_level(sda_pin) == 0; i++) {
        gpio_set_level(scl_pin, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl_pin, 1);
        esp_rom_delay_us(5);
    }

    // STOP condition: SDA rises while SCL is high
    gpio_set_level(sda_pin, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(5);

    Serial.printf("[I2C] Bit-bang recovery done — SDA now %s\n",
                  gpio_get_level(sda_pin) ? "released (high)" : "STILL LOW");
}

// ============================================================================
// init
// ============================================================================
bool init(const Config& config) {
    g_config = config;  // needed by logLineLevels()/recovery paths outside init()

    // Create mutex once — never destroyed (survives reinit cycles).
    if (g_bus_mutex == nullptr) {
        g_bus_mutex = xSemaphoreCreateRecursiveMutex();
    }

    logLineLevels("pre-init");
    // Must run before the I2C peripheral claims SDA/SCL — see comment above.
    bitBangClockRecovery((gpio_num_t)config.scl_pin, (gpio_num_t)config.sda_pin);
    logLineLevels("post-bitbang");

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

    // Basic connectivity check. A failure here after a reboot (rather than a
    // cold power-on) usually means a slave was left mid-transaction by whatever
    // caused the reboot and is still holding SDA low — an MCU-only reset does
    // not reset external chips. Clock it free before giving up: 9 SCL pulses
    // (resetBus()) is the standard I2C bus-recovery procedure, and it's exactly
    // what wake-from-standby already does successfully via reinit(). Retrying
    // here means a reboot that follows a bus wedge comes back up working
    // instead of staying crippled until a manual power cycle.
    if (!ping(EXIO_DEVICE)) {
        Serial.println("[I2C] WARNING: EXIO device not responding — attempting clock recovery");
        system_logger::warn("I2C", "EXIO not responding at init — attempting recovery");
        logLineLevels("pre-resetBus");
        resetBus();
        vTaskDelay(pdMS_TO_TICKS(10));
        logLineLevels("post-resetBus");
        if (!ping(EXIO_DEVICE)) {
            Serial.println("[I2C] WARNING: EXIO device still not responding after recovery");
            system_logger::error("I2C", "EXIO still not responding after recovery attempt");
        } else {
            Serial.println("[I2C] EXIO recovered after clock pulses");
            system_logger::info("I2C", "EXIO recovered after clock pulses");
        }
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
    esp_err_t last_err = ESP_OK;
    uint32_t total_us = 0;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) {
            g_stats.retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = i2c_master_transmit_receive(
            dev._handle,
            &reg, 1,          // write: register address
            data, len,        // read: response data
            pdMS_TO_TICKS(50) // timeout
        );
        total_us += (uint32_t)(esp_timer_get_time() - t0);

        if (err == ESP_OK) {
            success = true;
            break;
        }
        last_err = err;
    }

    // Latency watch: a transaction taking >20ms is the earliest signal of a
    // degrading (not yet fully wedged) bus — log it regardless of outcome.
    // See docs/i2c.md, "Known Historical Issues" (FT-06 freeze investigation).
    if (total_us > 20000) {
        system_logger::logf(system_logger::Level::WARN, "I2C",
            "SLOW %s reg=0x%02X %s dur=%luus task=%s",
            dev.name, reg, success ? "read" : "read-FAIL",
            (unsigned long)total_us, pcTaskGetName(nullptr));
    }

    if (!success) {
        Serial.printf("[I2C] %s reg=0x%02X read failed: %s (dur=%luus task=%s t=%lu)\n",
                      dev.name, reg, esp_err_to_name(last_err),
                      (unsigned long)total_us, pcTaskGetName(nullptr), (unsigned long)millis());
        system_logger::logf(system_logger::Level::WARN, "I2C",
            "FAIL %s reg=0x%02X read err=%s dur=%luus task=%s consec=%lu",
            dev.name, reg, esp_err_to_name(last_err), (unsigned long)total_us,
            pcTaskGetName(nullptr), (unsigned long)(g_stats.consecutive_failures + 1));
    }

    recordDeviceOp(dev, success, total_us);
    xSemaphoreGiveRecursive(g_bus_mutex);
    if (!success) {
        g_stats.failed_ops++;
        g_stats.consecutive_failures++;
    } else {
        g_stats.consecutive_failures = 0;
    }
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
    esp_err_t last_err = ESP_OK;
    uint32_t total_us = 0;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) {
            g_stats.retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = i2c_master_transmit(
            dev._handle,
            buf, len + 1,
            pdMS_TO_TICKS(50)
        );
        total_us += (uint32_t)(esp_timer_get_time() - t0);

        if (err == ESP_OK) {
            success = true;
            break;
        }
        last_err = err;
    }

    // Latency watch — see the matching comment in read() above.
    if (total_us > 20000) {
        system_logger::logf(system_logger::Level::WARN, "I2C",
            "SLOW %s reg=0x%02X %s dur=%luus task=%s",
            dev.name, reg, success ? "write" : "write-FAIL",
            (unsigned long)total_us, pcTaskGetName(nullptr));
    }

    if (!success) {
        Serial.printf("[I2C] %s reg=0x%02X write failed: %s (dur=%luus task=%s t=%lu)\n",
                      dev.name, reg, esp_err_to_name(last_err),
                      (unsigned long)total_us, pcTaskGetName(nullptr), (unsigned long)millis());
        system_logger::logf(system_logger::Level::WARN, "I2C",
            "FAIL %s reg=0x%02X write err=%s dur=%luus task=%s consec=%lu",
            dev.name, reg, esp_err_to_name(last_err), (unsigned long)total_us,
            pcTaskGetName(nullptr), (unsigned long)(g_stats.consecutive_failures + 1));
    }

    recordDeviceOp(dev, success, total_us);
    xSemaphoreGiveRecursive(g_bus_mutex);
    if (!success) {
        g_stats.failed_ops++;
        g_stats.consecutive_failures++;
    } else {
        g_stats.consecutive_failures = 0;
    }
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

// ⚠️ Despite the name and ESP-IDF's own docs, this does NOT send clock-recovery
// pulses on ESP32-S3 — i2c_master_bus_reset()'s wait loop polls a function the
// IDF LL layer stubs to `return false` unconditionally on this SoC, so it
// returns ESP_OK without ever driving a pulse. Kept only for the FSM-level
// reset it does perform (distinct from clock recovery); real bus-clear recovery
// is bitBangClockRecovery(), invoked from init() before the peripheral exists.
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
    system_logger::warn("I2C", "Full re-initialization triggered");

    // Acquire bus mutex — blocks any in-flight read()/write() from other tasks
    // until reinit completes. Mutex is recursive so init() → ping() inside here
    // can re-acquire without deadlocking. Bounded (not portMAX_DELAY): this is
    // called from the bus-health watchdog's own recovery arm, so a mutex holder
    // stuck in the exact hang this recovery exists to clear (e.g. the IDF NACK
    // busy-wait) must not be able to block recovery forever.
    if (g_bus_mutex != nullptr) {
        if (xSemaphoreTakeRecursive(g_bus_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("[I2C] reinit: bus mutex not acquired in 1000ms — skipping, will retry");
            return false;
        }
    }

    // Captured BEFORE teardown, while the peripheral still owns the pins — the
    // most useful sample, since it's closest to whatever condition triggered
    // the reinit in the first place.
    logLineLevels("pre-reinit-teardown");

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

    // ⚠️ This scan used to report ~61 phantom devices and was believed for months.
    // Measured 2026-07-31: back-to-back i2c_master_probe() calls return a false
    // ESP_OK on ALTERNATE calls. The evidence is the address list itself — hits
    // landed on every second address (0x0A 0x0C 0x0E 0x10 ... 0x7E) with occasional
    // phase slips, on a healthy, freshly power-cycled device whose UI, touch and
    // beacon all worked. No physical bus produces that pattern; a strict alternation
    // is an artifact of the probe loop, not 61 slaves.
    //
    // Two guards, because a diagnostic that lies is worse than no diagnostic — this
    // one actively misled a freeze investigation toward "wedged bus":
    //   1. a settle delay between probes, so each starts from an idle bus;
    //   2. double confirmation — a real slave ACKs every time it is asked, a phantom
    //      only on the alternating beat, so requiring two consecutive ACKs rejects it.
    // The raw count is printed alongside the confirmed one: if they ever diverge
    // again, the artifact is back and the scan is not to be trusted.
    Serial.println("==== I2C Bus Scan ====");
    int found = 0;
    int raw_hits = 0;
    for (uint8_t addr = 1; addr < 0x7F; addr++) {
        if (i2c_master_probe(g_bus_handle, addr, pdMS_TO_TICKS(5)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        raw_hits++;
        vTaskDelay(pdMS_TO_TICKS(2));
        if (i2c_master_probe(g_bus_handle, addr, pdMS_TO_TICKS(5)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;   // one ACK only — the alternation artifact, not a device
        }
        found++;
        const char* name = "Unknown";
        for (const auto& d : known) {
            if (d.addr == addr) { name = d.name; break; }
        }
        Serial.printf("  0x%02X  -  %s\n", addr, name);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (found == 0) {
        Serial.println("  No devices found! Check wiring.");
    } else {
        Serial.printf("Found %d device(s)\n", found);
    }
    if (raw_hits != found) {
        Serial.printf("  (%d single-ACK hits rejected as probe artifacts — see comment at scanBus)\n",
                      raw_hits - found);
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
