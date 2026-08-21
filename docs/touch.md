# Touch Integration Guide - CST820 Controller

## Overview

Capacitive touch via a CST820 (CST8xx-family) controller, I2C address `0x15`, read through [`i2c_manager`](i2c.md) — no direct register access anywhere outside the driver itself.

**If touch "works briefly then dies," or the chip NACKs everything after boot, read the Auto-Sleep
section below first** — that failure mode locked the project for two days (2026-08-20/21) and was
misdiagnosed as failing hardware before being root-caused as a chip firmware behavior.

## ⚠️ Auto-Sleep: the chip NACKs all I2C while idle (root-caused 2026-08-21)

**The single most important fact about the CST8xx family**: some chip firmware batches auto-enter
standby after **~1-2 seconds without a finger on the panel**, and while asleep the chip **NACKs every
I2C transaction** — on the bus it is indistinguishable from a dead or disconnected chip. Only two
things wake it: a physical touch, or a TP_RST pulse. Waveshare's own CST820 demo writes register
`0xFE` ("DisAutoSleep") right after reset for exactly this reason; this project didn't, going all the
way back to the first Arduino-era commit, and got away with it only because the original board's chip
happens not to sleep aggressively. Boards from other batches (two independent reports, 2026-08-20/21)
sleep on schedule, which produced two days of symptoms that all looked like failing hardware:

- **"Recovers after TP_RST, fails again within 1-2s"** — not a marginal FPC connector (the leading
  theory at the time); it was the auto-sleep timer expiring. The tight, repeatable timing was the tell.
- **"CST820 not found" at boot on warm reboots** — an `esp_restart()` doesn't power-cycle the chip,
  so it enters the new session still asleep and NACKs the init probe. (A USB reflash *does*
  power-cycle it, which is why touch "mysteriously" worked right after flashing.)
- **Touch dead all session on builds with the 10-failure circuit breaker (`cde07d3`)** — the breaker
  counted the sleeping chip's NACKs, tripped ~1s after boot, and disabled polling *permanently* — so
  the one path that could have revealed a live chip (a finger press waking it) was never polled again.
- **Spurious full-bus "wedge" recoveries and a mid-session crash/reboot** — the I2C health watchdog's
  single global failure counter was saturated by the sleeping chip alone and declared a healthy bus
  wedged. See [ADR-0034](adr/0034-touch-nack-throttle-not-permanent-breaker.md).

**The fix (three parts, all load-bearing — see ADR-0034 for why each alternative was rejected):**

1. **`cst820_disable_auto_sleep()`** writes `0xFE` = `0xFF` via `i2c_manager::writeByte()`. The
   setting is **volatile — it does not survive a chip reset**, so it must be re-issued while the chip
   is awake at every point where the chip may have just come back: end of `initTouch()` (after a
   successful probe/recovery), after the TP_RST toggle in `standby_manager`'s wake path, and when the
   read callback sees the chip respond again after a failure streak. **Do not remove any of these
   re-arm sites** — each covers a reset path the others don't.
2. **The read-failure circuit breaker throttles instead of disabling** (`lvgl_touch_read_cb()`,
   `device_manager.cpp`): after 10 consecutive failed reads it drops to one retry per 250ms rather
   than stopping permanently. A NACKing chip may just be asleep, and the finger that wakes it is
   exactly the read that must not have been given up on; 4Hz still catches a real press (~130-190ms)
   within a retry or two while keeping bus noise negligible. On the first successful read it logs
   recovery, resumes full rate, and re-arms DisAutoSleep.
3. **The I2C wedge detector requires ≥2 distinct failing devices** (`checkI2CBusHealth()`,
   `task_manager.cpp`) before declaring the bus wedged — a real wedge (slave holding SDA low) fails
   *every* device, so one noisy high-rate device can no longer trigger full-bus reinits on its own.

Field-verified on the affected board 2026-08-21: touch works and stays working. The FPC-connector
theory is retired; no hardware fault existed.

## Driver

`include/hardware/display/cst820.h` / `src/hardware/display/cst820.cpp` — deliberately thin, three functions:

**Boot-time recovery (2026-08-12, extended 2026-08-20/21)**: `device_manager::initTouch()` calls
`cst820_begin()`, whose `i2c_manager::ping()` is a bare single 10ms probe with no retry — unlike the
retried `read()`/`write()` path everything else on the bus uses. If that first ping fails,
`initTouch()` escalates: full bus reinit (`i2c_manager::reinit()` — must be `reinit()`, not
`resetBus()`, see [i2c.md](i2c.md)'s Bus Recovery section), then a TP_RST pulse (EXIO pin 1,
active-low, 15ms low + 100ms boot wait) to hard-reset the chip itself — which is also what wakes a
chip that entered the session asleep (see Auto-Sleep above; this is the common case on warm reboots,
not an exotic one). Whichever path succeeds, `initTouch()` then disables auto-sleep before
registering the LVGL input device.

```cpp
bool cst820_begin(uint8_t i2c_addr = 0x15);   // pings i2c_manager::TOUCH_DEVICE
bool cst820_read(CST820Point &pt, uint8_t i2c_addr = 0x15);
bool cst820_disable_auto_sleep();             // write DisAutoSleep (0xFE); re-issue after any chip reset

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

1. **Failure throttle** — after 10 consecutive failed reads, polling drops to one retry per 250ms
   until a read succeeds again (see the Auto-Sleep section above for why this must never be a
   permanent disable). Logs `[TOUCH] 10 consecutive read failures — throttling touch polling to
   250ms` on entry and `[TOUCH] Controller responding again — full-rate polling resumed` on recovery.
2. **Standby guard** — touch is not polled at all while in standby (`standby_manager::isStandby()`). Continuous 100Hz I2C reads with the display off were found to corrupt the controller's internal state over time, so the callback short-circuits to `LV_INDEV_STATE_RELEASED` instead.
3. **Scale to 480** — `scale_to_480()` maps the controller's raw range onto the 480×480 panel, picking the divisor by bracketing the raw value (`≤600` passthrough, `≤1500` → `/1023`, else `/4095`) rather than trusting a fixed controller spec.
4. **Clamp to screen bounds**, then **circular mask** — touches outside the round visible display area (`dx²+dy² > R²`, `R` = half screen width minus 2px margin) are dropped as a release, not passed through. This is the touch-side counterpart to the round-vs-square bounds fix in [FT-09](../ROADMAP.md) — the panel is physically round, and a square bounding check alone would accept presses in the framebuffer's corners that are actually under the bezel.
5. Sets `navigation::getNavState().touch_x/y/pressed` for the rest of the app, and calls `standby_manager::notifyUserActivity()` on every real press to reset the inactivity timer.

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
