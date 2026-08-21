#include "cst820.h"
#include "i2c_manager.h"

// CST820 touch controller — reads via i2c_manager (thread-safe by design in ESP-IDF 5.x)

bool cst820_begin(uint8_t i2c_addr) {
    // i2c_addr parameter kept for API compatibility but we use the pre-registered TOUCH_DEVICE
    (void)i2c_addr;
    return i2c_manager::ping(i2c_manager::TOUCH_DEVICE);
}

// Some CST8xx firmware batches auto-enter standby after ~1-2s without touch and
// NACK every I2C transaction while asleep (only a touch or TP_RST wakes them) —
// indistinguishable on the bus from a dead/disconnected chip. Waveshare's own
// CST820 demo writes DisAutoSleep (0xFE, any non-zero value) right after reset
// for exactly this reason. Must be called while the chip is awake (i.e. right
// after a successful probe or TP_RST), and re-issued after any chip reset —
// the register does not survive one.
bool cst820_disable_auto_sleep() {
    return i2c_manager::writeByte(i2c_manager::TOUCH_DEVICE, 0xFE, 0xFF);
}

// CST8xx packet at registers 0x01-0x06:
// 0x01: gesture, 0x02: point count, 0x03: XH, 0x04: XL, 0x05: YH, 0x06: YL
// Coordinates are 12-bit: X = ((XH & 0x0F) << 8) | XL
bool cst820_read(CST820Point &pt, uint8_t i2c_addr) {
    (void)i2c_addr;

    uint8_t d[7];
    if (!i2c_manager::read(i2c_manager::TOUCH_DEVICE, 0x01, d, sizeof(d))) {
        return false;
    }

    uint8_t P = d[1];
    pt.pressed = (P > 0);

    uint16_t x = ((d[2] & 0x0F) << 8) | d[3];
    uint16_t y = ((d[4] & 0x0F) << 8) | d[5];

    pt.x = x;
    pt.y = y;

    return true;
}
