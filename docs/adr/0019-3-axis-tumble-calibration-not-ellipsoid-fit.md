# ADR-0019: 3-axis calibration via a second min/max tumble step, not an ellipsoid/soft-iron fit

Status: Accepted
Date: 2026-08-02
Decided by: Claude (design + implementation), you (field data + go/no-go inputs from WP-3)

## Context

WP-4 (Level 1 health metrics) and ADR-0018 (real tilt compensation required) both converge on the same
next step: `docs/compass_calibration_foundation.md` WP-5, giving the compass a real `cal_z_offset`.
A flat 360° spin — the existing calibration procedure — structurally cannot produce one: the Z axis
keeps pointing at the same part of the sky for the entire spin, so `min ≈ max` and the offset is
unrecoverable (`compass_qmc5883l.h`'s `cz` comment, `docs/compass_calibration_foundation.md` §3.4).
Field sample 8 (`freeform`, `docs/calibration/wp3_results.md`) confirmed a tumble/figure-8 motion
*can* cover the sphere on this hardware (elevation −87.6°..+82.5°, azimuth the full −180°..180° in
~60s) — feasibility was answered, not the calibration math itself.

Two designs were on the table for turning that coverage into an offset:

1. **Extend the existing min/max-per-axis approach to Z**, run during a new tumble step, exactly as
   X/Y already work — `cal_z_offset = (max_z + min_z) / 2`.
2. **Fit a 3-D ellipsoid** (hard-iron center + a 3×3 soft-iron correction matrix) to the tumble's
   point cloud via least squares — the more complete correction a phone's compass app typically does.

A third question, folded into the same decision: does the flat-spin baseline (`H0`, circle-fit
residual, axis ratio — WP-4, feeding `classifyHealth()`) survive, get replaced, or get combined with
the new tumble data?

## Decision

1. **Min/max per axis, not an ellipsoid fit.** WP-3's own data argues against the extra complexity:
   axis ratio on flat360 samples was 1.06–1.17 (`docs/calibration/wp3_results.md` "H₀ and the tilt
   threshold" table) — soft iron is measurably present but modest, and §3.4 had already flagged it as
   "probably minor" before the trip. A full ellipsoid fit adds a 3×3 matrix to store, apply on every
   `read()` call, and validate — for a correction this project's own field data says is secondary to
   the hard-iron offset that min/max already captures well (`H0` repeatable to ~1% across two
   sessions). It would also be new, unverified math on embedded hardware with no field data
   characterizing whether the *fit* itself is well-conditioned from a hand-tumbled sweep, as opposed to
   whether the sweep's raw coverage is adequate (which sample 8 did establish). Matching the existing,
   already-correct X/Y pattern is lower-risk and reuses code the field trip already validated.
2. **Keep the flat-spin baseline (`H0`/residual/axis-ratio) as Step 1, unchanged, and add the tumble
   as Step 2 — not a single combined motion.** `classifyHealth()`'s tilt detector (WP-4) is only
   physically meaningful when `H0` was measured while the device was actually flat — that's what
   makes "h_mag differs from H0" mean "tilted" rather than "differently oriented." A tumble never holds
   that condition, so there is no way to extract an equivalent baseline from tumble data without either
   an accelerometer-gated "was this moment flat" filter (adding WP-6's dependency to WP-5) or a WMM
   inclination-based derivation from the tumble's total-field magnitude (a new, unverified
   cross-calibration). Keeping Step 1 as-is sidesteps both and costs nothing — the existing flow
   already produces exactly what Level 1 needs.
3. **3-D coverage is scored from the magnetometer alone (elevation + azimuth in sensor frame), not
   from the accelerometer.** The accelerometer measures device tilt; what actually matters for whether
   the tumble adequately sampled the field sphere is the *magnetic* vector's spread in sensor
   coordinates, which is a different quantity (device tilt and magnetic-vector-in-sensor-frame track
   together only if the field were vertical, which it isn't — inclination is ~59.7° locally). Sample
   8's own feasibility analysis (`wp3_results.md` "Freeform (013)") computed exactly this quantity from
   `cx/cy/cz` with no accelerometer involved, so scoring it the same way on-device needed no new sensor
   dependency and stayed consistent with how feasibility was originally established.

## Consequences

**Easier**: WP-5 shipped with no new field data collection and no new fit algorithm — it is the same
proven min/max primitive, wired to a second sweep and a second set of coverage thresholds. The Save
button's final computation is unchanged for X/Y and mechanically identical for Z. `classifyHealth()`
and its NVS-persisted quality metrics are untouched, so no downstream consumer of `H0`/residual/axis
ratio needed to change.

**Harder / given up**: soft-iron correction remains absent. If a future calibration turns out to have
markedly worse axis ratio than the 1.06–1.17 flat-sample band (a bent enclosure, a different
mounting), there's no mechanism here to correct for it — recalibration is the only lever, same as
today. If that turns out to matter, it is a new WP, not a retrofit onto this one: an ellipsoid fit
would need its own field validation of fit stability from a hand-tumbled sweep, which this decision
explicitly deferred rather than assumed.

**Sets up WP-6 cleanly**: Level 3 (tilt compensation) needs a real `cal_z_offset` and can now assume
one exists once the user has completed calibration; it does not need to reason about whether Level 2
attempted a soft-iron correction, because it didn't.
