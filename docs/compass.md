# Compass — QMC5883L Implementation Guide

**Status**: Complete ✅ | Last updated: 2026-08-11

**This doc covers the QMC5883L (BH-880 module) specifically** — register map, calibration, health
classification, WMM declination. As of 2026-08-11, an HMC5883L (BN-880 module) is also supported via a
separate low-level driver (`compass_hmc5883l.cpp`, see [`docs/bh880_module.md`](bh880_module.md)'s
"Compass: HMC5883L Magnetometer" section for its register map). Everything below this point —
calibration storage, health classification, the 2-axis heading formula — is chip-agnostic and applies
identically to both chips once raw X/Y/Z are read; only the register-level `begin()`/raw-read logic
differs per chip. See [ADR-0033](adr/0033-compass-internal-dispatch-not-rename.md) for the dispatch
design.

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

Hard-iron offsets compensate for static magnetic fields from the device's own components (battery, metal chassis, etc.). They are determined by a two-step calibration procedure (WP-5, `docs/compass_calibration_foundation.md` §12).

**Calibration values** are stored in NVS (`cal_cx`, `cal_cy`, `cal_cz`) and loaded at boot via `compass_qmc5883l::setCalibration()`.

**Serial commands:**
```
compass status    — chip ID + current state
compass read      — single X/Y/Z reading + heading
compass stream N  — stream for N seconds (default 5s)
```

**Calibration procedure**:
- **Step 1 (flat spin)**: slowly rotate the device through 360° on a flat surface. Records min/max on X/Y and computes `cal_x`/`cal_y = (max+min)/2`, plus the `H0`/residual/axis-ratio metrics below. A flat spin alone cannot calibrate Z — the axis never changes what it points at, so `min ≈ max` — which is why there's a second step.
- **Step 2 (tumble)**: tumble the device through all orientations — flip it end over end, figure-8 motion. Records min/max on Z the same way, and gates the Save button on 3-D coverage: elevation span, azimuth sector count (8×45°), and Z span must all clear an OK/GOOD threshold before saving is allowed, so a tumble that only rocks side-to-side doesn't pass by accident. Coverage is scored from the magnetometer alone (elevation/azimuth of the corrected vector in sensor frame) — no accelerometer needed. See [ADR-0019](adr/0019-3-axis-tumble-calibration-not-ellipsoid-fit.md) for why this is min/max-per-axis rather than a full ellipsoid/soft-iron fit.

`cal_z` is applied by `read()`. The `atan2f(cy, cx)` formula above is the flat-hold case; when the
device is tilted, Level 3 tilt compensation (WP-6, shipped and closed 2026-08-02/2026-08-06 — see
ROADMAP.md and [`compass_calibration_foundation.md`](compass_calibration_foundation.md) §12) replaces
it with an accelerometer-corrected formula in `src/navigation/tilt_compensation.cpp`, so `cal_z` does
feed the final heading. A known, accepted cosmetic limitation remains: a fast flat→nose-up tilt
produces a ~30° transient bounce that self-corrects within ~1s (accel-only, no gyro — ADR-0018).

---

## Level 1 Health Metrics (WP-4)

The same 360° sweep that computes hard-iron offsets also captures three quality metrics, stored in
NVS alongside them (`cal_h0`, `cal_resid`, `cal_axr`):

- **`H0`** — mean horizontal-field semi-axis radius, `((max_x-min_x)+(max_y-min_y))/4`. Roughly
  constant for a given location (≈3000 raw LSB in LA) — the baseline live readings are compared
  against.
- **Circle-fit residual** — RMS deviation of the sweep's magnitude from `H0`, as a percentage.
  Healthy band: 2–4%.
- **Axis ratio** — `span_x/span_y`. Should be ≈1.0 for a circular locus; healthy band ~1.06–1.07.
  A persistent departure indicates soft iron or per-axis scale error.

`compass_qmc5883l::classifyHealth(data, h0)` uses `H0` at runtime: it smooths `h_mag` with a ~1s EMA
and applies hysteresis (enter/exit 1.12/1.08) to classify each reading as `HEALTHY`, `TILTED` (h_mag
elevated — tilt only ever inflates it), `DISTURBANCE` (sensor overflow only), or `UNCALIBRATED` (no
`H0` yet). **This is the health/trust indicator, not the correction path** — the radar HUD shows a
"Compass: hold flat" / "Compass: interference" / "Compass: recalibrate?" indicator, hidden when
healthy, independent of the actual tilt correction applied to the heading (Level 3 tilt compensation,
WP-6 — see above).

**⚠️ Corrected 2026-08-02, same day as ship**: the first version also tried a low-magnitude threshold
(ratio < 0.85) for `DISTURBANCE`, guessing a disturbance might weaken the field. Reported unreliable
in the field within hours — "interference" fired inconsistently walking near metal objects. The guess
was never field-verified and is probably backwards for the common case: a nearby ferromagnetic object
concentrates field lines, which *inflates* `h_mag` in the same direction as tilt, not the opposite.
`DISTURBANCE` is now sensor-overflow-only — a hardware fact, not a threshold guess — until someone
logs `h_mag` walking past a real disturbance (§8.3 sample 7, `disturbance` label) and derives an
actual threshold from it.

The "recalibrate?" case comes from the **stored** calibration's own residual/axis-ratio score, not
from live dynamics — telling a stale calibration apart from a momentary tilt using a single live
reading isn't something the field data supports doing reliably.

**Serial command**: `compass cal` now prints `H0`, residual, axis ratio, and the live classification
alongside the pre-existing offset-magnitude heuristic.

**Full derivation and field numbers**: [`compass_calibration_foundation.md`](compass_calibration_foundation.md) §5,
[`calibration/wp3_results.md`](calibration/wp3_results.md).

### When to recalibrate

Hard-iron calibration is a **fixed geometric correction** — it nulls out a magnetic bias from
components that travel with the sensor (nearby ferrous/magnetized material, internal wiring
position). It does not drift with time, temperature, or location on its own, so under normal use one
calibration lasts indefinitely. Re-run it when:

- **The enclosure is opened** — even just the faceplate. Disassembly/reassembly can shift internal
  wiring enough to change the bias (project memory `compass_recal_after_case_open.md`: this produced
  non-90° cardinal deltas and a field magnitude that varied noticeably with heading — fixed by
  recalibrating).
- **Anything magnetized/ferrous is added, moved, or removed** near the board — a new mount, a
  magnetic case clasp, a different battery.
- **Physical shock or damage** that could shift internal components.
- **The HUD's "Compass: recalibrate?" indicator lights up** — driven by the *stored* calibration's
  own residual/axis-ratio score (§ above), not live dynamics.

**Not a reason to recalibrate**: changing location. That's magnetic declination (WMM, below), which
is computed automatically every session from the GPS fix — no user action needed.

**Gap to be aware of**: the live classifier (`TILTED`) cannot fully distinguish "the calibration has
gone stale" from "you're just holding it at an angle" from a single reading — §5.1 of the foundation
doc explains why that distinction needs data this project doesn't have. If the device reads `TILTED`
while held flat and away from obvious magnetic sources, treat that as a recalibration hint too, even
without a case-open event. (`DISTURBANCE` is unambiguous — it only fires on sensor-reported overflow,
a genuinely strong nearby source — but correspondingly won't catch a milder disturbance; see the
correction above.)

**One-time migration note**: a calibration saved *before* this feature existed has no `H0` baseline
(`compass_cal_h0 == 0` in NVS), so the HUD will show "Compass: recalibrate?" once after updating to
this firmware even though the existing offsets still work fine. Recalibrating once clears it and
populates `H0`/residual/axis-ratio going forward.

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
