# ADR-0009: NimBLE instead of Bluedroid

Status: Accepted
Date: 2026-03-20
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

After BLE initialization, the Bluedroid stack left only ~2–7KB SRAM free at runtime — it consumed
~65KB. That was not enough for SD card DMA buffers (~4–16KB), so every SD log flush failed with
`sdmmc_read_blocks failed (257)`. The same low-memory condition fragmented the LVGL heap badly enough
to make double-press button detection unreliable. This was acute resource exhaustion, not a tuning
question — SD logging and input handling were both breaking in the field (CHANGELOG, "SRAM
Optimization — NimBLE Migration", 2026-03-20).

## Decision

Replace Bluedroid with NimBLE (`h2zero/NimBLE-Arduino@^1.4.0` at the time; the library is now vendored
directly as `https://github.com/h2zero/esp-nimble-cpp.git#v1.4.1` in `platformio.ini`, carried over
unchanged through the later ESP-IDF migration). NimBLE's runtime footprint is ~25KB SRAM vs
Bluedroid's ~65KB — a measured ~40KB freed.

## Consequences

**Easier**: SD card logging stopped failing, double-press detection recovered (heap stalls gone), and
the project carries ~40KB of SRAM headroom whenever BLE is active that did not exist before. This
headroom was later a precondition for other work — see `memory/project_wifi_ble_sram.md`.

**Harder**: NimBLE's API differs enough from Bluedroid's that `beacon_proximity.cpp` needed a full
rewrite of its callback layer (`BLEAdvertisedDeviceCallbacks` → `NimBLEAdvertisedDeviceCallbacks`,
`onResult(BLEAdvertisedDevice)` → `onResult(NimBLEAdvertisedDevice*)`, scan-completion polling
changed from timer-based to `g_pScan->isScanning()`). The library also needs **two manual source
patches** to build against ESP-IDF 5.5 that are not part of the upstream v1.4.1 tag and are lost if
`.pio/libdeps/` is ever wiped: a missing `#include <time.h>` in `NimBLEAttValue.h` (used
unconditionally but only included behind a config flag), and a replacement for
`esp_nimble_hci_and_controller_deinit()` in `NimBLEDevice.cpp`, which IDF 5.x removed (see
`memory/feedback_nimble_patches.md`). Separately, NimBLE and WiFi cannot coexist in this board's SRAM
budget — `esp_wifi_init()`'s ~60–80KB allocation doesn't fit alongside NimBLE and task stacks, so WiFi
was made session-only and `beacon_proximity::deinit()` must run before `wifi_manager::init()`
(`memory/project_wifi_ble_sram.md`).

**Gave up**: nothing algorithmic — all beacon business logic (EMA, zones, hysteresis, trend, sonar)
carried over unchanged (CHANGELOG). What was given up is the relative simplicity of depending on the
more widely-used, less patch-dependent Bluedroid stack.
