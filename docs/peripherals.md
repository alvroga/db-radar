# Peripherals Guide — RTC and GPS

## Real-Time Clock (PCF85063)

I2C address `0x51`, accessed through [`i2c_manager`](i2c.md) via `RTC_DEVICE`. Driver: `include/hardware/sensors/rtc_pcf85063.h` / `src/hardware/sensors/rtc_pcf85063.cpp`.

```cpp
namespace rtc {
struct Time {
    int year, month, day, hour, minute, second, wday;
    bool valid;   // plausible AND marked-initialized
};

bool begin(uint8_t i2c_addr = 0x51);
bool read(Time& t);
bool set(const Time& t);
bool set_from_compile_time();
bool set_from_epoch(uint32_t epoch, int tz_offset_minutes = 0);
bool is_initialized();
void clear_initialized();
}
```

**"Initialized" is tracked separately from "readable."** The chip has a spare RAM byte (register `0x03`) used purely as a magic-value flag (`0xA5`, written by every successful `set()`). `read()` returns `valid = false` whenever that flag isn't set, even if the register read itself succeeded and the time looks plausible — this is what stops the chip's power-on-reset default time from being treated as real. `is_initialized()` / `clear_initialized()` expose that flag directly for callers that need to check or force a re-sync (e.g. after a battery-backup failure).

**Plausibility, not correctness, is what `read()`/`set()` validate** — year 2000-2099, month 1-12, day 1-31, hour/minute/second in range. A wrong-but-plausible time (wrong day, right format) passes; the magic-byte check is the only thing gating "was this chip ever actually set."

**Time sources**: `set_from_compile_time()` (parses `__DATE__`/`__TIME__` as a last-resort fallback) and `set_from_epoch()` (used by NTP/GPS time sync, `src/utils/ntp_sync.cpp` — includes its own Gregorian calendar math since there's no `<time.h>` dependency here). GPS-derived time sync takes priority over compile-time in practice; compile-time is the boot-time fallback if nothing better is available yet.

**Queued access from the I2C Task**: in addition to the direct `rtc::read()`/`rtc::write()` calls above (used at boot and from diagnostic tooling), `task_manager.cpp` also handles RTC reads and a GPS-driven one-shot `RTC_TIME_SET` as queued `I2COperation` requests, consistent with this project's queue-based I2C access pattern for cross-task safety (see the Task Management section of [CLAUDE.md](../CLAUDE.md)).

## GPS (Beitian BH-880, LC76G, or BN-880)

Full hardware detail, wiring, and the multi-module design: [`bh880_module.md`](bh880_module.md). Summary relevant to integration:

- UART on GPIO43 (TX)/GPIO44 (RX). One firmware image supports all three modules — **BH-880** (UBX
  binary protocol, NAV-PVT), **LC76G**, or **BN-880** (both NMEA/PAIR text protocol, same parser) —
  auto-identified on first boot, then pinned as a persisted choice (Settings > GPS, or serial `gps
  module set ...`) so every later boot skips detection entirely. See
  [ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md).
- Position only — heading is **not** derived from GPS on any module. On a BH-880 or BN-880, a compass
  on the same module is the sole heading source (QMC5883L or HMC5883L respectively, chosen from the
  pinned module with no runtime probing — see the Navigation Modes section of [CLAUDE.md](../CLAUDE.md));
  GPS heading fusion was removed from the codebase entirely. The LC76G has no compass at all, so a
  board running one is forced to North-Up navigation automatically.
- Parsing entry point: `src/hardware/sensors/gps_bh880.cpp`, namespace `gps_bh880` (handles both
  protocols despite the filename — kept for call-site stability, see the module doc).
- The compass side of the BH-880 shares the I2C bus (`COMPASS_DEVICE`, address `0x0D`) — see [`i2c.md`](i2c.md) for the full device list and [`compass.md`](compass.md) for compass-specific calibration/tilt-compensation detail.

## Code References

- RTC: `include/hardware/sensors/rtc_pcf85063.h`, `src/hardware/sensors/rtc_pcf85063.cpp`
- RTC-driven time sync: `src/utils/ntp_sync.cpp`
- GPS: `src/hardware/sensors/gps_bh880.cpp`, full guide [`bh880_module.md`](bh880_module.md)
- I2C transport for both: [`i2c.md`](i2c.md)
