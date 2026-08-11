# ADR-0033: Compass internal chip dispatch, not a public namespace rename

Status: Accepted
Date: 2026-08-11
Decided by: Claude (proposed, user approved)

## Context

Adding BN-880 support (see [ADR-0032](0032-pinned-gps-module-not-always-auto-detect.md)'s addendum)
required a second compass chip driver — an HMC5883L, alongside the existing QMC5883L driver in
`compass_qmc5883l.cpp`/`.h`. `compass_qmc5883l::` is called from five other files across the codebase
(`task_manager.cpp`, `settings_screen.cpp`, `navigation.cpp`, `diagnostics.cpp`, `tilt_bench.cpp`),
all for chip-agnostic operations: read a heading, classify health, calibrate. None of them care which
physical chip is actually present.

## Decision

Keep `compass_qmc5883l.{h,cpp}` as the single public compass entry point, unchanged in name, with the
same function signatures every existing caller already uses. It gains an internal `ChipType`
(`QMC5883L`/`HMC5883L`), set once by `device_manager::initCompass()` from the pinned GPS module
selection (never runtime-probed — see ADR-0032's addendum for why). `begin()`/`read()` branch
internally on that state to the right chip's register I/O; calibration storage, EMA health
classification, and the 2-axis `atan2f(cy, cx)` heading formula stay exactly as they were, since both
chips populate the same `CompassData` fields before that shared code runs.

The actual HMC5883L register work (chip ID check, config registers, X-Z-Y burst order, big-endian byte
order) lives in a new, genuinely separate file, `compass_hmc5883l.{h,cpp}` — matching this project's
existing one-driver-per-physical-chip convention (e.g. `accel_qmi8658.cpp` is its own file, not folded
into a shared "sensors" file). That file is intentionally low-level only: no calibration, no health
classification, no public API beyond `begin()`/`isReady()`/`readRaw()`, and it is not meant to be
called from anywhere except `compass_qmc5883l.cpp`.

**Alternative rejected: rename to a chip-agnostic namespace** (e.g. `compass::`), with
`compass_qmc5883l.cpp`/`compass_hmc5883l.cpp` becoming private chip backends behind it. Cleaner
semantically — the name `compass_qmc5883l` orchestrating two chips is a real, if minor, misnomer,
flagged with a top-of-file comment rather than fixed. Rejected because it would require touching all
five existing call sites for a naming-clarity gain with no functional benefit, and the project's own
convention (avoid touching what doesn't need touching; three similar lines beat a premature
abstraction) argues against a repo-wide rename to satisfy naming purity alone. If this pairing ever
grows to a third or fourth chip, or the two-chip orchestration inside one file gets genuinely hard to
follow, revisit — the rename is still available as a follow-up, this ADR doesn't foreclose it
permanently.

## Consequences

- **Makes easier**: adding BN-880 touched zero of the five existing compass call sites. Any future
  caller keeps writing `compass_qmc5883l::read(data)` exactly as before, unaware of which chip answers.
- **Makes harder**: a reader encountering `compass_qmc5883l.cpp` for the first time has to notice the
  top-of-file comment to learn it also drives an HMC5883L — the filename alone doesn't tell them.
- **Gave up**: a fully accurate module name. Traded for a smaller diff and zero risk of missing a call
  site during the rename.
