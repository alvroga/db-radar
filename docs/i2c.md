# I2C Bus Management Guide

## Overview

All I2C traffic on this board goes through one unified manager — `i2c_manager` (`include/hardware/i2c/i2c_manager.h`, `src/hardware/i2c/i2c_manager.cpp`) — built on ESP-IDF 5.x's new `driver/i2c_master.h` API. There is no legacy driver layer to reconcile: earlier in the project's history a `TCA9554PWR.cpp`/`exio.cpp` pair and a Wire-based `I2C_Driver.cpp` compatibility shim existed alongside it; both are gone. Every device read/write, from any task, goes through this module.

**Shared I2C Bus**: SDA=15, SCL=7, **100kHz** — set by `system_config::communication::I2C_FREQ_HZ`
(`include/core/system_config.h`), passed through `device_manager::initI2C()` to `i2c_manager::init()`.
`i2c_manager::Config::frequency`'s own struct default is 400kHz, but nothing calls `init()` with a
default-constructed `Config` — the 100kHz value is what actually runs. See "Bus Speed" below for why
it's not 400kHz and whether it needs to be.

## Devices on the Bus

Six devices are registered at `init()`, in this order (also the order `getDeviceStats()` fills its output array):

| Handle | Address | Device |
|---|---|---|
| `IMU_DEVICE_LOW` | 0x6A | QMI8658 accelerometer (primary) |
| `IMU_DEVICE_HIGH` | 0x6B | QMI8658 accelerometer (alt address) |
| `RTC_DEVICE` | 0x51 | PCF85063 real-time clock |
| `TOUCH_DEVICE` | 0x15 | CST820 touch controller |
| `EXIO_DEVICE` | 0x20 | TCA9554 IO expander |
| `COMPASS_DEVICE` | 0x0D | QMC5883L compass (on the BH-880 GPS/compass module) |

The IMU is genuinely on this bus and in active use — it drives the accelerometer-based tilt compensation shipped in [ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md). (An older note elsewhere claimed the IMU had been removed to reduce bus contention with GPS — that's stale; GPS is a UART peripheral, not I2C, so there was never a shared-bus conflict between the two, and the IMU has stayed on this bus throughout.)

## API

```cpp
#include "i2c_manager.h"

// Read/write — serialized internally by a FreeRTOS recursive mutex,
// safe to call concurrently from any task (UI, I2C, System, ...)
bool ok = i2c_manager::read(i2c_manager::RTC_DEVICE, reg, buf, len);
bool ok = i2c_manager::write(i2c_manager::RTC_DEVICE, reg, buf, len);

// Single-byte convenience wrappers
uint8_t v;
i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, reg, v);
i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, reg, v);

// Availability check (no data transfer)
bool present = i2c_manager::ping(i2c_manager::TOUCH_DEVICE);
```

All four take an optional `retries` parameter (default 3).

### IO Expander (TCA9554)

```cpp
i2c_manager::exio::State state;
i2c_manager::exio::begin(state);
i2c_manager::exio::set(i2c_manager::exio::BUZZER, false, state);
```

Pin map (`i2c_manager::exio::Pin`) — this is the corrected mapping; see "Critical pin-mapping history" below for why it's called out:

```cpp
LCD_RST = 0,
TP_RST  = 1,
LCD_CS  = 2,
EXIO3   = 3,
EXIO4   = 4,
EXIO5   = 5,
EXIO6   = 6,
BUZZER  = 7
```

## Error Handling and Statistics

- **Cross-device stats** (`i2c_manager::getStats()`): total ops, failed ops, retry count, and `consecutive_failures` — back-to-back failures across *any* device, reset on the next success anywhere. A sustained run here (not one device's occasional miss) is a wedged bus — a slave holding SDA low — and is what the recovery watchdog ([ADR-0003](adr/0003-proactive-i2c-bus-recovery-watchdog.md)) acts on.
- **Per-device forensic counters** (`getDeviceStats()`, added 2026-08-02 for the FT-06 freeze investigation): ops, fails, consecutive fails, worst single-transaction latency, and time since last failure, per device. Exists because a wedge that starts on *one* device (e.g. a marginal compass connection) looks identical to the cross-device stats until it's bad enough to fail everything — by then the ramp that would have identified the culprit is gone. Full detail: [`i2c_bus_freeze_investigation.md`](i2c_bus_freeze_investigation.md).

## Bus Recovery

- `resetBus()` — FSM-level reset. **Despite its name and ESP-IDF's own docs, this does not send clock-recovery pulses on ESP32-S3** — see the comment above its definition in `i2c_manager.cpp`.
- `reinit()` — full re-initialization: tears down all device handles and the bus, then re-inits. This *does* run real bit-bang clock recovery on the freed pins before recreating the bus. Use this (not `resetBus()`) after standby wake or when the I2C controller FSM is stuck.

## Bus Speed: 100kHz, not 400kHz — and it's safe to test 400 again

Dropped 400kHz → 100kHz on 2026-08-02 as a diagnostic/mitigation experiment (T-B in the old freeze
investigation log) on the theory that marginal signal integrity was causing the FT-06 freezes. **The
real root cause turned out to be unrelated** — the ESP-IDF NACK-hang driver bug below, patched the
same day — so the 100kHz change was never actually evaluated on its own merits, and nobody has
reverted it since. It costs nothing functionally at 100kHz: a compass read is ~1ms and the bus is
still >95% idle at the current 10Hz sensor rate.

**Current call**: leave it at 100kHz. The system has been stable at this speed since 2026-08-02, and
reverting now would only reintroduce an unknown against a device that's currently running clean — no
performance need is pushing for 400kHz. One data point that keeps it an open question rather than a
closed one: 100% of the residual (patch-independent) I2C failures are on the compass specifically —
the one device reached over a cable — and front-loaded in the first hour after boot (see
[Open Questions](i2c_bus_freeze_investigation.md#open-questions)), consistent with either a real
signal-integrity margin issue or an unrelated power-on transient. Nobody has data at 400kHz *since*
the NACK-hang patch landed, so it's untested whether that failure rate would change either way.

**If it's ever worth testing**: flip `system_config::communication::I2C_FREQ_HZ` back to `400000` for
one field session and compare `getDeviceStats()`'s per-device fail counts (especially the compass)
against the 100kHz baseline above. This is now a clean, low-risk experiment — the NACK-hang patch
bounds any hang regardless of bus speed, so a bad result at 400kHz can no longer manifest as a freeze,
only as a higher (but bounded and retried) failure rate.

## Known Historical Issues (Resolved)

- **I2C NACK hang** — a real ESP-IDF driver bug (unbounded busy-wait after a NACK, matching upstream `espressif/esp-idf#17720`), not application logic. Patched at build time. See [`i2c_bus_freeze_investigation.md`](i2c_bus_freeze_investigation.md) and [ADR-0021](adr/0021-i2c-nack-hang-build-time-backport.md).
  - **Still open, lower priority**: *why* the bus produces NACKs at all is unconfirmed. Field data narrows it though — failures are compass-only (the one device reached over a cable) and front-loaded in the first hour after boot, then clean for the rest of the session (a warm-up transient, not wear-out). See the freeze investigation doc's [Open Questions](i2c_bus_freeze_investigation.md#open-questions).
- **`I2C_PROCESS_MS` floor** — 20ms is a tuned hard floor, not an arbitrary value; halving it broke the button and buzzer via bus contention with the touch driver. See [ADR-0013](adr/0013-i2c-process-ms-tuned-floor.md).
- **Critical pin-mapping history**: during the original consolidation into `i2c_manager`, the EXIO pin map was briefly wrong (`BUZZER = 0, LCD_CS = 1, LCD_RST = 2`), causing the buzzer to stick on and boot failures. The pin map documented above under "IO Expander" is the corrected, current mapping.

## Code References

- Manager: `include/hardware/i2c/i2c_manager.h`, `src/hardware/i2c/i2c_manager.cpp`
- Touch driver (thin wrapper over the manager): `src/hardware/display/cst820.cpp` — see [`touch.md`](touch.md)
- RTC driver: `src/hardware/sensors/rtc_pcf85063.cpp` — see [`peripherals.md`](peripherals.md)
- Accelerometer: `src/hardware/sensors/accel_qmi8658.cpp` (self-contained, own register constants, routes reads/writes through `i2c_manager`)
