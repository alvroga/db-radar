# ADR-0020: Tilt-compensation frame rotation and sign resolved from bench self-consistency, not the textbook formula or a first-principles sign argument

Status: Accepted
Date: 2026-08-02
Decided by: Claude (analysis), you (bench data collection + field verification)

## Context

WP-6 ([[ADR-0018]]) needed two things the bench protocol (`docs/compass_tilt_bench.md`) couldn't hand
over directly: a heading formula, and the global sign of the mag↔accel frame rotation once the
signed-permutation search (48 candidates) picked a matrix. Two off-the-shelf options existed for the
formula: the textbook roll/pitch decomposition (`atan2(ax,ay,az)` → roll/pitch → tilt-compensated
`atan2`), and a first-principles argument for the sign (standard accelerometer axis convention +
LA's known magnetic inclination).

Both were tried first and both failed against `tiltbench_001.csv` (2 passes × 6 poses, 2026-08-02):
the textbook formula gave ~69° circular std across poses — unusable — because its roll/pitch axis
labeling assumes a specific accel-axis-to-physical-rotation convention that didn't match this
hardware's actual mounting. The first-principles sign argument couldn't be checked against anything
until the permutation itself was known, and once it was, disagreed with what the data supported.

## Decision

1. **Use a coordinate-convention-agnostic vector formula instead of the textbook one**: normalize the
   gravity estimate to get "down", cross it with a fixed device reference axis (accel Y, the physical
   top of the screen) to get "east", cross again for "north", project the frame-rotated mag vector
   onto that basis, `atan2`. This needed no assumption about which physical rotation a given accel
   axis represents — it only assumes gravity and the reference axis aren't parallel — and fit the
   non-degenerate bench poses (flat×2, edge-down×2) to ~4° across two passes.
2. **Resolve the permutation's global sign from bench self-consistency, not the accelerometer-sign/
   inclination argument.** Two tests were used together: (a) the sign that keeps computed heading
   consistent across the four non-degenerate poses, and (b) the sign under which the tilt-compensated
   formula reduces *exactly* to the existing `atan2(cy, cx)` 2-axis formula at zero tilt, with no
   residual offset — meaning it doesn't require re-deriving the already-tuned WMM declination. Both
   pointed to the same sign, which was the opposite of the first-principles guess. The bench evidence
   won because it is tied to this chip's actual measured behavior, not an assumed textbook convention
   — consistent with this project's standing rule (§3.3, the WMM sign, the compass mount check) that
   signs get fixed on hardware, not on paper.
3. **Keep the sign runtime-flippable** (`compass tilt sign +1|-1`, session-only, not NVS-persisted)
   rather than committing to it purely from the bench analysis, since the bench data alone — however
   internally consistent — was never checked against a *known* real-world heading. That live check is
   still open (see `include/navigation/tilt_compensation.h`).

## Consequences

**Easier**: the formula has one clean singularity (reference axis parallel to gravity, i.e. pointed
straight up/down) instead of the several branch-dependent edge cases a roll/pitch decomposition
carries, and it's the one already-guarded in `computeHeading()`. Zero-tilt backward compatibility with
the existing declination calibration came for free instead of needing a second offset constant.

**Harder**: the nose-up/nose-down bench poses — specifically chosen by the protocol to pin down the
mag-X↔accel-Y pairing — don't validate against this particular formula's own self-consistency check,
because they sit at its singularity. That's an artifact of the formula choice, not a defect in those
poses or a reason to distrust the permutation itself (which four other poses validated tightly, and
which was originally extracted from the raw sensor values independent of any heading formula).

**Gave up**: full confidence in the sign from a single desk-based analysis alone. It shipped as a
reversible default rather than a hardcoded constant pending a live check — that check happened
2026-08-02 (hand-held flat/nose-up/recovery testing showed the heading settling back to the correct
value, not a steady ~180° offset), confirming the bench-derived sign. The runtime flip stays available
regardless, per this project's standing rule of keeping empirically-fixed signs field-adjustable.
