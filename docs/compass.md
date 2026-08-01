# Compass — QMC5883L Implementation Guide

**Status**: Complete ✅ | Last updated: 2026-03-18

## Hardware Overview

The QMC5883L 3-axis magnetometer is built into the Beitian BH-880 module. Hardware details are in [`docs/bh880_module.md`](bh880_module.md). Key facts:

| Property | Value |
|----------|-------|
| Chip | QMC5883L |
| I2C address | 0x0D |
| Bus | Shared I2C bus via `i2c_manager` (SDA=GPIO15, SCL=GPIO7, 400kHz) |
| ODR configured | 200Hz continuous |
| Range | 2 Gauss |
| OSR | 512 (best noise rejection) |
| Output | 16-bit signed X, Y, Z |

---

## Physical Mounting & Axis Orientation

The BH-880 PCB is mounted inside the enclosure. The compass chip axes have been **empirically verified** by pointing the device at 4 cardinal directions and confirming the heading output is consistent and correct.

**Result**: No software mounting offset is required. The standard `atan2(Y, X)` formula produces correct magnetic heading without rotation correction. The measured residual offset (~12°) is entirely explained by magnetic declination, not mounting error.

**Enclosure orientation**: The device display is rotated 90° CCW in the enclosure; the compass axes happen to still resolve correctly with the standard formula in this orientation.

---

## Heading Computation

```cpp
// Hard-iron calibration applied
int16_t cx = x_raw - cal_x_offset;
int16_t cy = y_raw - cal_y_offset;

// Magnetic heading (degrees, 0=magnetic north, clockwise)
float heading = atan2f((float)cy, (float)cx) * 180.0f / M_PI;
if (heading < 0) heading += 360.0f;
```

This gives **magnetic heading**. Magnetic declination is then added (see below) to produce true heading.

---

## Hard-Iron Calibration

Hard-iron offsets compensate for static magnetic fields from the device's own components (battery, metal chassis, etc.). They are determined by a 360° rotation calibration procedure.

**Calibration values** are stored in NVS (`cal_cx`, `cal_cy`, `cal_cz`) and loaded at boot via `compass_qmc5883l::setCalibration()`.

**Serial commands:**
```
compass status    — chip ID + current state
compass read      — single X/Y/Z reading + heading
compass stream N  — stream for N seconds (default 5s)
```

**Calibration procedure**: Slowly rotate device through 360° on a flat surface. The calibration code records min/max per axis and computes offsets as `(max + min) / 2`.

---

## Magnetic Declination (WMM)

After hard-iron calibration, a consistent residual offset (~12° in LA) remains. This is **magnetic declination** — the difference between magnetic north and true geographic north.

**Solution**: World Magnetic Model (WMM) auto-correction. At first GPS fix each session, declination is computed from GPS lat/lon/date and applied to every compass reading:

```cpp
true_heading += compass_declination_deg;  // += for this hardware (not -=)
```

**Sign convention**: East declination is **added** (not subtracted) on this device. The QMC5883L axes on the BH-880 produce headings that read *low* of true north, so East declination corrects upward. Empirically confirmed in Los Angeles (12.25° East, 2026).

**Full WMM documentation**: [`docs/wmm_declination.md`](wmm_declination.md)

---

## Full Pipeline

```
QMC5883L hardware (200Hz, 2G, 512 OSR)
  → System Task reads every 100ms (SYSTEM_UPDATE_MS — 10Hz)
  → Hard-iron offsets applied (cal_x, cal_y from NVS)
  → atan2(cy, cx) → magnetic heading
  → WMM declination added → true heading
  → COMPASS_UPDATE queued to UI Task
  → EMA smoothing (HEADING_SMOOTHING α=0.3 at 10Hz)
  → 0.5° render deadband on the smoothed heading
  → ui.current_heading → radar rotates
```

**Reaction time**: ~100ms after physical rotation (10Hz read rate).
**Smoothing**: α=0.3 EMA at 10Hz — re-derived from α=0.8 when the rate went 1Hz → 10Hz, so the
settling *time* is preserved rather than the per-sample coefficient.

---

## I2C Bus Constraint

> ⚠️ **Corrected 2026-07-31.** The mechanism this section used to give — *"the LVGL CST820 touch
> driver calls `Wire.requestFrom()` directly, bypassing `i2c_mutex`"* — was written for the
> Arduino-core build and **does not describe the current ESP-IDF firmware**. There is no `Wire` usage
> in `src/` or `include/` at all; touch reads go through `i2c_manager::read()`
> (`src/hardware/display/cst820.cpp:19`) under the recursive `g_bus_mutex`, exactly like the RTC,
> EXIO and compass. **No participant on the bus is unprotected.** Full correction:
> [`compass_i2c_constraint.md`](compass_i2c_constraint.md).

### The Problem (as originally observed, on the Arduino build)

Attempting to read the compass more frequently (e.g., from the I2C Task at 50Hz) caused:
```
[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error -1
```
Followed by UI freezes and eventual crash. That error string is an Arduino `Wire` log line and cannot
be produced by the current firmware.

### Status under the current stack: untested, not disproved

Nobody has retried moving the compass read to the I2C Task since the ESP-IDF migration removed the
unprotected path. So the *reason* for the constraint is gone, but the *conclusion* has not been
re-tested — treat it as an open question rather than either a settled prohibition or a green light.
Devices and who reads them today:

| Device | Address | Who reads | Bus access |
|--------|---------|-----------|-----------|
| CST820 touch | 0x15 | UI Task (LVGL), ~11.7Hz | Via i2c_manager + mutex |
| QMC5883L compass | 0x0D | System Task, 10Hz | Via i2c_manager + mutex |
| PCF85063 RTC | 0x51 | I2C Task | Via i2c_manager + mutex |
| TCA9554 EXIO | 0x20 | I2C Task | Via i2c_manager + mutex |

If it is picked up, measure rather than reason: move the read behind a runtime flag and watch
`i2c_manager::getStats()` (`total_ops`, `failed_ops`, `consecutive_failures`) plus task health. Note
the one hard adjacent data point — raising the **I2C Task's own rate** from 20ms to 10ms did break the
button and buzzer on hardware (backlog §8.1b), and *that* failure is also un-root-caused. The bus is
still worth respecting; the old explanation for why just isn't evidence.

### The hardware fix that was considered and dropped: a dedicated bus

Moving the compass wires to a second bus (GPIO19/20, the USB D+/D− pins, free only when
`ARDUINO_USB_CDC_ON_BOOT=0`) would have removed contention physically. **Not pursued** — it needs a
cable swap and it disables the serial monitor, which on this board is also the only way to run
diagnostics at all. The `cc-radar-compass` build environment that existed for it was **removed
2026-03-14**.

It is also no longer motivated by the read rate: the compass reads at **10Hz** today, from the System
Task, on the shared bus, without incident.

---

## Performance Summary

| Metric | Value |
|--------|-------|
| Read rate | 10Hz (`SYSTEM_UPDATE_MS = 100`) |
| Rotation reaction time | ~100ms |
| Heading EMA | α=0.3 (`HEADING_SMOOTHING`) |
| Render deadband | 0.5° on the smoothed heading |

---

## Key Code Files

| File | Purpose |
|------|---------|
| `src/hardware/sensors/compass_qmc5883l.cpp` | Driver: init, read, calibration |
| `include/hardware/sensors/compass_qmc5883l.h` | CompassData struct, public API |
| `src/utils/task_manager.cpp` | System Task compass read + WMM application |
| `src/utils/wmm_declination.cpp` | WMM2020 declination computation |
| `include/hardware/i2c/i2c_manager.h` | COMPASS_DEVICE handle, setCompassBus() |
