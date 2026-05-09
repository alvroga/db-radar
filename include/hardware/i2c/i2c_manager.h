#ifndef I2C_MANAGER_H
#define I2C_MANAGER_H

#include <cstdint>
#include <stddef.h>
#include "driver/i2c_master.h"  // ESP-IDF 5.x new I2C master API (thread-safe)

namespace i2c_manager {

// Device handle — wraps ESP-IDF i2c_master_dev_handle_t internally.
// After init(), _handle is valid for all read/write calls.
// Pass by reference (DeviceHandle&) because _handle is set in-place.
struct DeviceHandle {
    uint8_t addr;
    const char* name;                    // For logging
    i2c_master_dev_handle_t _handle = nullptr;  // Set by init(), not by caller
};

struct Config {
    int sda_pin = 15;
    int scl_pin = 7;
    uint32_t frequency = 400000;
};

// Initialize I2C bus and register all devices (call once during setup)
bool init(const Config& config = Config{});

// Core I2C operations — serialized by an internal FreeRTOS recursive mutex.
// Safe to call from any task concurrently (UI Task, System Task, etc.).
bool read(DeviceHandle& dev, uint8_t reg, uint8_t* data, size_t len, int retries = 3);
bool write(DeviceHandle& dev, uint8_t reg, const uint8_t* data, size_t len, int retries = 3);

// Single-byte convenience functions
bool readByte(DeviceHandle& dev, uint8_t reg, uint8_t& value, int retries = 3);
bool writeByte(DeviceHandle& dev, uint8_t reg, uint8_t value, int retries = 3);

// Device availability check
bool ping(DeviceHandle& dev);

// Full I2C bus scan (prints all detected devices)
void scanBus();

// Bus recovery — sends 9 SCL pulses to unstick any slave holding SDA low.
bool resetBus();

// Full re-initialization: tears down all device handles and bus, then re-inits.
// Use after standby wake — more reliable than resetBus() when the I2C controller
// FSM is stuck and physical recovery pulses alone are not enough.
bool reinit(const Config& config = Config{});

struct Stats {
    uint32_t total_ops = 0;
    uint32_t failed_ops = 0;
    uint32_t retry_count = 0;
};
const Stats& getStats();

// Common device handles (defined in i2c_manager.cpp, registered at init)
extern DeviceHandle IMU_DEVICE_LOW;   // 0x6A
extern DeviceHandle IMU_DEVICE_HIGH;  // 0x6B
extern DeviceHandle RTC_DEVICE;       // 0x51
extern DeviceHandle TOUCH_DEVICE;     // 0x15
extern DeviceHandle EXIO_DEVICE;      // 0x20
extern DeviceHandle COMPASS_DEVICE;   // 0x0D (QMC5883L on BH-880)

// TCA9554 IO Expander (was exio.cpp, now consolidated here)
namespace exio {
    enum Pin : uint8_t {
        LCD_RST = 0,
        TP_RST  = 1,
        LCD_CS  = 2,
        EXIO3   = 3,
        EXIO4   = 4,
        EXIO5   = 5,
        EXIO6   = 6,
        BUZZER  = 7   // CRITICAL: Buzzer is pin 7, not 0!
    };

    struct State {
        uint8_t out = 0xFF;
    };

    bool begin(State& state);
    bool set(Pin pin, bool high, State& state);
    bool writeOutput(const State& state);
    bool readInput(uint8_t& value);
    bool readOutput(uint8_t& value);
    bool rawWrite(uint8_t reg, uint8_t value);
    bool rawRead(uint8_t reg, uint8_t& value);

    static constexpr uint8_t REG_INPUT    = 0x00;
    static constexpr uint8_t REG_OUTPUT   = 0x01;
    static constexpr uint8_t REG_POLARITY = 0x02;
    static constexpr uint8_t REG_CONFIG   = 0x03;
}

} // namespace i2c_manager

#endif // I2C_MANAGER_H
