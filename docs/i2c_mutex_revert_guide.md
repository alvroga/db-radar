# I2C Mutex Fix — Revert Guide

**Commit**: `38e89df`  
**Branch**: `esp-idf`  
**Date**: 2026-04-13

## What was changed and why

The commit added a FreeRTOS recursive mutex to `i2c_manager` to serialize all I2C
bus access. Root cause: ESP-IDF's internal I2C bus lock was not reliably preventing
concurrent `i2c_master_transmit_receive` calls from the UI Task (CST820 touch, 10ms)
and System Task (QMC5883L compass, 100ms) from colliding on the shared bus, causing
`ESP_ERR_INVALID_STATE` and triggering 5-failure soft-reset cycles (~3s compass dead
period per occurrence).

Also fixed a sleep/wake race where the System Task would call `compass_qmc5883l::reset()`
while `restoreActivePowerSettings()` was still mid-reinit (each `delay()` in that
function yields, allowing the System Task to slip in).

## Known remaining issue after this fix

The QMC5883L and GPS chip (B1301N) share power on the BH-880 module. During GPS
acquisition (no fix), the GPS RF frontend causes periodic current spikes that
momentarily corrupt the I2C bus, causing 4× NACK cascades → two soft-reset cycles
(~3s each). This is **hardware-level** and unaffected by the mutex. It only occurs
during the GPS searching phase; once GPS settles the errors reduce to occasional
single NACKs that self-recover on retry.

## Nuclear revert (undo everything)

```bash
git revert 38e89df
git push
```

This reverses all 7 files in one shot. Compass will go back to constant collision
errors but the GPS-related errors also disappear (compass rate back to 1Hz means
fewer reads = fewer chances to land on a GPS spike).

## Surgical revert (keep parts, drop others)

### 1. Remove the I2C mutex (highest impact — removes the core fix)

**`src/hardware/i2c/i2c_manager.cpp`**
- Remove `#include "freertos/FreeRTOS.h"` and `#include "freertos/semphr.h"`
- Remove the `static SemaphoreHandle_t g_bus_mutex = nullptr;` declaration
- In `init()`: remove the mutex creation block
- In `read()`: remove the `xSemaphoreTakeRecursive` / `xSemaphoreGiveRecursive` calls
- In `write()`: same
- In `ping()`: same  
- In `reinit()`: remove the Take at the top and Give at the bottom
- In `scanBus()`: same

**`include/hardware/i2c/i2c_manager.h`**
- Revert comment on `read()`/`write()` to: `// Core I2C operations — thread-safe via i2c_master API (no external mutex needed)`

### 2. Revert compass read rate (if needle feels too slow / GPS spikes too frequent)

**`src/utils/task_manager.cpp`** line ~1340:
```cpp
// Revert from:
if (compass_now - s_last_compass_read >= 20) {
// Back to:
if (compass_now - s_last_compass_read >= 100) {
```

### 3. Revert sleep/wake delay (if standby wake is causing problems)

**`src/utils/task_manager.cpp`** in the `s_was_in_standby` branch (~line 1317):
- Remove `vTaskDelay(pdMS_TO_TICKS(500));`

### 4. Revert System Task rate

**`include/utils/task_manager.h`**:
```cpp
// Revert from:
static constexpr uint32_t SYSTEM_UPDATE_MS = 200;
// Back to:
static constexpr uint32_t SYSTEM_UPDATE_MS = 1000;
```

## Performance after this fix vs before

| Metric                     | Before `38e89df`         | After `38e89df`              |
|----------------------------|--------------------------|------------------------------|
| Collision errors           | Constant (every session) | Gone                         |
| Sleep/wake I2C errors      | Every wake, accumulating | Gone                         |
| GPS acquisition errors     | Constant (masked by above) | ~2 soft-reset cascades then rare |
| Compass response latency   | ~1000ms worst case       | ~220ms worst case            |
| Radar needle update rate   | ~1fps effective          | ~5fps                        |
| Compass dead periods       | Frequent, ~5s each       | Rare, ~3s each, GPS-caused only |

## Verdict

If field testing shows the GPS acquisition cascades are acceptable (they only occur
during satellite searching and self-recover), keep the fix. If they are disruptive,
the surgical revert of just the compass read rate (item 2 above) reduces the
probability of landing on a GPS spike without removing the mutex protection.
