# ADR-0017: Compass as the sole heading source, replacing GPS heading fusion

Status: Accepted
Date: 2026-03-20
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

Heading-up navigation mode originally derived heading from GPS course-over-ground (NMEA RMC fields
7-8, speed + course; CLAUDE.md, `docs/navigation_modes.md`). GPS-derived course is only meaningful
while the receiver is actually moving fast enough for the fix to have a reliable direction component —
the old design needed a 0.5-knot minimum-speed threshold and a 10-second stationary timeout that fell
back to north-up when the user stopped, because GPS heading is noise at walking pace or standstill.
That's a poor match for a handheld device: a user standing still trying to orient themselves is
exactly when heading needs to work, not exactly when it degrades.

The Beitian BH-880 GPS module adoption (`[Unreleased] - 2026-03-20`, CHANGELOG.md) bundled a QMC5883L
magnetometer on the same board (I2C address 0x0D) as a drop-in replacement for the prior Quectel
LC76G. A magnetometer has the opposite failure mode from GPS course: it works fine at rest and needs
no minimum speed, but it brings its own two costs GPS heading didn't have — hard-iron calibration and
magnetic declination correction.

## Decision

Make the QMC5883L compass the sole heading source and remove GPS heading fusion from `navigation.cpp`
entirely (CHANGELOG.md:948, "GPS heading fusion removed"). GPS continues to supply *position* — over
**UBX NAV-PVT binary** (`src/hardware/sensors/gps_bh880.cpp`, `UBX_NAV_PVT`, `sendUBX(0x06, 0x01, ...)`
enabling PVT output), not NMEA — but no longer contributes to the heading calculation at all. The
System Task reads the compass and queues `COMPASS_UPDATE`; the UI Task applies it to
`ui.current_heading`, which now drives the N indicator, on-screen waypoint rotation, and off-screen
indicators (`docs/navigation_modes.md`).

## Consequences

**Easier**: heading is available and correct at zero speed — no minimum-speed threshold, no
stationary-timeout fallback to north-up. The read rate has since gone from ~1Hz (original design,
CHANGELOG.md:951) to 10Hz (`SYSTEM_UPDATE_MS = 100`, `docs/compass.md`), independent of GPS fix quality
or motion.

**Harder**: a magnetometer needs **hard-iron calibration** — a 360° rotation procedure recording
min/max per axis and storing the offset in NVS (`docs/compass.md:47-60`,
`compass_qmc5883l.cpp`) — that a GPS-only design never required. That calibration is **invalidated by
opening the enclosure** (even just the faceplate), per project memory
(`compass_recal_after_case_open.md`), because nearby ferrous/magnetic components shift. It also needs
**magnetic declination correction**, computed once per session from the WMM2020 spherical-harmonic
model (n=1..3, ±1° accuracy) at first GPS fix, persisted to NVS, and applied as
`true_heading += declination` — a sign convention confirmed empirically (East declination reads *low*
on this hardware without the correction; ~12.25° East in the Los Angeles area, `docs/compass.md:64-74`,
CHANGELOG.md:965-973) rather than derived, which is itself a smaller instance of the project's general
"measure RF/sensor behavior, don't just reason about it" pattern (see ADR-0015's calibration note for
the same principle applied to beacon DF).

**Gave up**: nothing GPS heading uniquely provided — GPS course-over-ground degrades at low speed in
exactly the situations a magnetometer handles well, so there was no accuracy regime being traded away,
only the two calibration/declination costs above being taken on in exchange for working at rest.
