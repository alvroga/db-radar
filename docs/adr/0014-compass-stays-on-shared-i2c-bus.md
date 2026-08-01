# ADR-0014: Compass stays on the shared I2C bus rather than moving to a second bus

Status: Accepted
Date: 2026-03 (precise day not recoverable — see Context)
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The QMC5883L compass (0x0D) shares GPIO15/7 with the CST820 touch controller, the PCF85063 RTC, and
the TCA9554 IO expander. GPIO19/20 (`Wire1`) — the USB D-/D+ pins, usable as I2C only when
`ARDUINO_USB_CDC_ON_BOOT=0` — were available as a dedicated second bus, and infrastructure for it
(`i2c_manager::setCompassBus(&Wire1)`, a `cc-radar-compass` PlatformIO environment) was built and then
removed. This is a decision *not* to change something, which the project already treats as ADR-worthy
(see ADR-0002's "null decision").

**The original reasoning is stale, and that is the load-bearing fact for this ADR.** The documented
justification — `docs/compass_i2c_constraint.md`, still checked in — was that the compass "cannot" be
read from the I2C Task because the LVGL CST820 driver called `Wire.requestFrom()` directly, bypassing
`i2c_mutex`, so raising the compass's read rate collided with unprotected touch reads
(`[E][Wire.cpp:499] requestFrom() Error -1`). That description is accurate only for the pre-ESP-IDF
Arduino build. A grep of `src/` and `include/` for `\bWire\b` today finds no live call sites; touch
now reads through `i2c_manager::read()` under `g_bus_mutex`, exactly like the compass, RTC, and EXIO
(`src/hardware/display/cst820.cpp:19`). **The move has not been retried since the migration**, so the
conclusion — compass isolation is necessary — is *untested under the current stack, not disproved*.
`docs/compass.md` and `docs/compass_i2c_constraint.md` both carry a 2026-07-31 correction to this
effect but leave the original analysis in place as historical record.

A second, independent, and still-current cost was found in `docs/compass.md:141-145`: GPIO19/20 are
the USB D-/D+ pins, so claiming them for a second I2C bus requires `ARDUINO_USB_CDC_ON_BOOT=0`, which
disables the USB CDC serial monitor — the project's only means of running diagnostics (per-project
memory: "serial monitor requires USB… cannot run serial diagnostics on battery-only power"). That cost
holds regardless of whether the original contention reasoning was ever valid. The `cc-radar-compass`
build environment that would have exercised the second bus was removed (memory:
`cc-radar-compass env removed (2026-03-14) — not pursuing cable-swap approach`); this is the most
concrete date available for "decided not to pursue," but it lives only in session memory notes, not in
`CHANGELOG.md` or recoverable git history (the repository's pre-2026-05-09 history was squashed into a
single "Initial public release" commit, so no per-commit date survives for it). Hence the `2026-03`
month-level date above rather than an invented day.

## Decision

Leave the compass on the shared bus (GPIO15/7, System Task, 10Hz) rather than moving it to a dedicated
`Wire1` bus on GPIO19/20. Compass reads have run at 10Hz on the shared bus without incident since the
BH-880 module was adopted (ADR-0017).

## Consequences

**Easier**: no cable rework, no board-specific alternate wiring, and the USB CDC serial monitor stays
available at all times — which this project depends on for essentially all diagnostics.

**Harder**: the shared bus remains the single point of contention for touch, RTC, EXIO, and compass
together, and — see ADR-0013 — that bus has already shown it can break in ways nobody has fully
explained (the `I2C_PROCESS_MS` 10ms revert). Any future I2C-adjacent change has one more device to
account for.

**Gave up**: nothing conclusively — the isolation option was never truly foreclosed, only deprioritized
once its original justification evaporated and its remaining cost (losing the serial monitor) stopped
looking worth paying for an unconfirmed benefit. **This status must not be read as a settled
engineering conclusion.** If compass update latency or bus contention becomes a real problem again, the
cheap next step is a measurement — flip the read to the I2C Task behind a runtime flag and watch
`i2c_manager::getStats()` — not a rewrite, and not a re-assertion of the stale Arduino-era reasoning.
