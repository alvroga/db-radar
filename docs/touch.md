# Touch Integration Guide - CST820 Controller

## Overview

Capacitive touch via a CST820 (CST8xx-family) controller, I2C address `0x15`, read through [`i2c_manager`](i2c.md) — no direct register access anywhere outside the driver itself.

## Driver

`include/hardware/display/cst820.h` / `src/hardware/display/cst820.cpp` — deliberately thin, two functions:

**Boot-time recovery (2026-08-12)**: `device_manager::initTouch()` calls `cst820_begin()`, whose
`i2c_manager::ping()` is a bare single 10ms probe with no retry — unlike the retried `read()`/`write()`
path everything else on the bus uses. If that first ping fails, `initTouch()` now runs the same
clock-recovery-then-retry step `i2c_manager::init()` already does for `EXIO_DEVICE`
(`i2c_manager::resetBus()`, 10ms delay, one more ping) before reporting `Touch: FAIL`. Added after a
field report of unresponsive touch on a BN-880 board's first-boot picker screen, where the boot log
showed the accelerometer probe hitting a NACK burst moments before touch was pinged — see the
CHANGELOG.md entry for detail. Not yet confirmed against the reporting board.

```cpp
bool cst820_begin(uint8_t i2c_addr = 0x15);   // pings i2c_manager::TOUCH_DEVICE
bool cst820_read(CST820Point &pt, uint8_t i2c_addr = 0x15);

struct CST820Point {
    uint16_t x, y;   // raw controller coordinates
    bool pressed;
};
```

`cst820_read()` reads 7 bytes starting at register `0x01` in one transaction:

| Offset | Field |
|---|---|
| 0x01 | gesture |
| 0x02 | point count (`> 0` → pressed) |
| 0x03/0x04 | X, high/low (12-bit: `((XH & 0x0F) << 8) \| XL`) |
| 0x05/0x06 | Y, high/low, same packing |

Raw coordinate range varies by controller firmware — could be ~0..480, ~0..1023, or ~0..4095. The driver does not normalize this; that happens at the LVGL integration layer (below).

## LVGL Integration

`lvgl_touch_read_cb()` in `src/core/device_manager.cpp` is the LVGL input-device read callback. Per touch:

1. **Standby guard** — touch is not polled at all while in standby (`standby_manager::isStandby()`). Continuous 100Hz I2C reads with the display off were found to corrupt the controller's internal state over time, so the callback short-circuits to `LV_INDEV_STATE_RELEASED` instead.
2. **Scale to 480** — `scale_to_480()` maps the controller's raw range onto the 480×480 panel, picking the divisor by bracketing the raw value (`≤600` passthrough, `≤1500` → `/1023`, else `/4095`) rather than trusting a fixed controller spec.
3. **Clamp to screen bounds**, then **circular mask** — touches outside the round visible display area (`dx²+dy² > R²`, `R` = half screen width minus 2px margin) are dropped as a release, not passed through. This is the touch-side counterpart to the round-vs-square bounds fix in [FT-09](../ROADMAP.md) — the panel is physically round, and a square bounding check alone would accept presses in the framebuffer's corners that are actually under the bezel.
4. Sets `navigation::getNavState().touch_x/y/pressed` for the rest of the app, and calls `standby_manager::notifyUserActivity()` on every real press to reset the inactivity timer.

Rotation is handled upstream by LVGL itself, not by this callback — see the Render Pipeline section of [CLAUDE.md](../CLAUDE.md) for how touch coordinates relate to the current rotation mode.

## Coordinate Logging (troubleshooting)

`diag touch on|off` (serial, off by default on boot) prints every press as
`[TOUCH] raw=(x,y) lvgl=(x,y)` — the raw controller reading from `cst820_read()` alongside the final
scaled/clamped point handed to LVGL, throttled to 10Hz so holding a touch doesn't flood the console.
Gated by `diagnostics::DiagState::touch_log`, checked directly in `lvgl_touch_read_cb()`.

## Testing

After any display or rotation change, verify touch alignment by tapping all four screen "corners" (within the round visible area) and confirming taps land where expected — a rotation/touch mismatch shows up as taps registering offset or mirrored from the visible cursor position.

## Code References

- Driver: `include/hardware/display/cst820.h`, `src/hardware/display/cst820.cpp`
- LVGL callback: `src/core/device_manager.cpp` — `lvgl_touch_read_cb()`, `scale_to_480()`
- I2C transport: [`i2c.md`](i2c.md)
