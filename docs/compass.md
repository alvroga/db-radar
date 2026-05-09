# Compass — QMC5883L Implementation Guide

**Status**: Complete ✅ | Last updated: 2026-03-18

## Hardware Overview

The QMC5883L 3-axis magnetometer is built into the Beitian BH-880 module. Hardware details are in [`docs/bh880_module.md`](bh880_module.md). Key facts:

| Property | Value |
|----------|-------|
| Chip | QMC5883L |
| I2C address | 0x0D |
| Bus | Shared Wire (SDA=GPIO15, SCL=GPIO7, 400kHz) |
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
  → System Task reads every ~1s (Wire bus constraint — see below)
  → Hard-iron offsets applied (cal_x, cal_y from NVS)
  → atan2(cy, cx) → magnetic heading
  → WMM declination added → true heading
  → COMPASS_UPDATE queued to UI Task
  → EMA smoothing (α=0.8, 1Hz updates)
  → ui.current_heading → radar rotates
```

**Reaction time**: ~1 second after physical rotation (1Hz read rate).
**Smoothing**: α=0.8 EMA settles within ~3 seconds after a 90° turn.

---

## I2C Bus Constraint

### The Problem

Attempting to read the compass more frequently (e.g., from the I2C Task at 50Hz) immediately causes:
```
[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error -1
```
Followed by UI freezes and eventual crash.

### Root Cause

All I2C devices share one bus (GPIO15/7). The LVGL CST820 touch driver calls `Wire.requestFrom()` **directly** — it bypasses `i2c_manager` and never acquires `i2c_mutex`. Touch reads happen at ~60Hz from the UI Task on Core 1. Compass reads on a different task (Core 0) collide with these unprotected touch transactions.

| Device | Address | Who reads | Bus access |
|--------|---------|-----------|-----------|
| CST820 touch | 0x15 | UI Task (LVGL), ~60Hz | Direct Wire — no mutex |
| QMC5883L compass | 0x0D | System Task, ~1Hz | Direct Wire — no mutex |
| PCF85063 RTC | 0x51 | I2C Task | Via i2c_manager + mutex |
| TCA9554 EXIO | 0x20 | I2C Task | Via i2c_manager + mutex |

The mutex protects RTC/EXIO from each other, but **cannot protect against the unprotected touch reads**. Compass reads from any high-frequency task will randomly collide with touch reads.

### Why 1Hz Works

The System Task loops every 1000ms. A compass read takes ~1ms. Touch reads take ~1ms at 60Hz. The collision probability per compass read at 1Hz is very low (~0.1%). At 50Hz it becomes frequent (~5% per read = multiple errors per second).

### The Real Fix: Dedicated I2C Bus

Move compass wires to Wire1 (GPIO19/20 — currently USB D+/D-):
- GPIO19/20 are free when `ARDUINO_USB_CDC_ON_BOOT=0` (serial monitor unavailable)
- Move compass SDA → GPIO19, SCL → GPIO20
- Call `i2c_manager::setCompassBus(&Wire1)` (infrastructure already exists)
- Move compass read to I2C Task → 50Hz updates → smooth rotation

This approach was implemented in the `cc-radar-compass` build environment but not pursued because it requires a hardware cable swap and disables serial monitoring. The 1Hz/1s reaction time is acceptable for walking navigation.

---

## Performance Summary

| Metric | Current | With Wire1 upgrade |
|--------|---------|-------------------|
| Read rate | ~1Hz | ~50Hz |
| Rotation reaction time | ~1s | ~100ms |
| Serial monitor | ✅ Available | ❌ Disabled |
| Requires hardware mod | No | Yes (cable swap) |

---

## Key Code Files

| File | Purpose |
|------|---------|
| `src/hardware/sensors/compass_qmc5883l.cpp` | Driver: init, read, calibration |
| `include/hardware/sensors/compass_qmc5883l.h` | CompassData struct, public API |
| `src/utils/task_manager.cpp` | System Task compass read + WMM application |
| `src/utils/wmm_declination.cpp` | WMM2020 declination computation |
| `include/hardware/i2c/i2c_manager.h` | COMPASS_DEVICE handle, setCompassBus() |
