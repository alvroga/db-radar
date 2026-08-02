# ADR-0018: Real tilt compensation required, not a lookup table; gyro fusion deferred, not adopted

Status: Accepted
Date: 2026-08-02
Decided by: Claude (analysis), you (field trip + go/no-go confirmation)

## Context

`compass_qmc5883l.cpp`'s heading is 2-axis (`atan2f(cy, cx)`), and [[compass-tilt-inversion / ADR
groundwork]] `docs/compass_calibration_foundation.md` §3.3 had already shown this inverts past
~31.5° of tilt (= 90° − local magnetic inclination). What that analysis couldn't answer without field
data was *how bad* the error is at realistic (not vertical) phone-style holding angles, and whether a
cheaper fix than full tilt compensation — e.g. a fixed correction offset for "phone-style" — could
close the gap. WP-3 (`docs/calibration/wp3_results.md`) exists to answer exactly that, and two
candidate designs were on the table: (a) a static/lookup correction, cheaper to build and no new
sensor fusion; (b) real accelerometer-based tilt compensation (Level 3), which needs a properly
3-axis-calibrated magnetometer (WP-5) first. A third question, whether to add the gyro (already on the
same QMI8658 chip) to that fusion, was open per `docs/compass_calibration_foundation.md` §6A.2 and
§10.

The field trip (18 samples, `docs/calibration/`) gave two walk-straight, phone-style samples at
nearly identical tilt (~46–50°, confirmed via the accelerometer's `az`/`ay` split) but opposite walking
directions: heading error vs GPS course was **−135° walking north** and **+4° walking south**.

## Decision

1. **Reject the lookup-table/static-offset approach. Build real tilt compensation (Level 3, WP-6).**
   A −135°-vs-+4° swing at essentially the same tilt magnitude means the error is a function of
   *heading*, not just tilt angle — there is no single correction value a lookup table could hold. The
   error comes from the interaction between the tilt axis and LA's ~59.7° magnetic inclination, which
   rotates with heading; only a real accelerometer-derived roll/pitch feeding the standard
   tilt-compensated heading formula corrects it, and it needs the actual `cal_z_offset` from WP-5, not
   the currently-unused zero — so Level 3 still waits on Level 2 in the existing WP-4→5→6 order.
2. **Do not add the gyro to the tilt-fusion path now.** WP-3's shake spectrum (100Hz samples) shows
   accel-magnitude energy peaking at ~2Hz/~4Hz (walking cadence + 2nd harmonic) with ~40% above 5Hz —
   real content an accel-only path must reject by oversampling (50–100Hz) and averaging down rather
   than reading at a flat 10Hz. That averaging costs latency: rejecting content below ~1Hz means a
   filter time constant on the order of half a second to a second, which will lag genuine tilt changes
   (e.g. the "glancing occasionally" behavior in the walk-straight protocol). A gyro-aided
   complementary/Kalman filter would recover that latency without reopening the noise problem, because
   angular rate is unaffected by the linear shocks that dominate accel noise. That is a real,
   understood benefit — not a reason to build it yet. It adds continuous `CTRL7 = 0x43` bus traffic
   (vs. the current accel-only `0x01`) against a bus with an already-tuned timing floor and an open,
   undiagnosed freeze issue (ADR-0013, ROADMAP.md FT-06), an unmeasured power cost (datasheet check
   never done), and its own fusion constant (complementary-filter α or Kalman covariance) — exactly
   the class of constant this project has repeatedly gotten wrong on the first attempt (§9.1a's τ
   regression, the beacon EMAs, ADR-0011). **Deferred**, triggered by field evidence that the
   accel-only tilt estimate feels laggy in practice — not by a number these CSVs can answer, since none
   of them exercise rapid re-tilting.
   This is unrelated to, and does not reopen, ADR-0017's separate rejection of the gyro as a **heading
   (yaw)** source — that failure mode is unbounded drift with no absolute reference to correct against.
   Tilt (pitch/roll) has gravity as a permanent, driftless reference, so a gyro there is bounded and
   self-correcting in a way it structurally cannot be for yaw.

## Update (2026-08-02, post-implementation)

The predicted trigger fired, in a narrow form: a field test of the finished Level 3 formula
([[ADR-0020]]) showed a real ~30° transient heading bounce during a fast flat→nose-up tilt, caused by
the gravity τ-EMA lagging the near-instantaneous mag reading mid-transition — recovering correctly
within roughly a second once the motion stopped. This is exactly "does the accel-only tilt estimate
lag in practice," but the response was to lower `GRAVITY_EMA_TAU_S` 1.0s → 0.5s
(`src/navigation/tilt_compensation.cpp`) rather than build gyro fusion — the bounce is bounded, fast,
self-correcting, and only shows up during a deliberate fast re-tilt, not the kind of persistent lag
this ADR treated as the actual gyro trigger. Gyro fusion remains deferred; the trigger condition is
now a known, observed behavior rather than a hypothetical, and a future occurrence worse than "recovers
within ~1s" is the thing that would actually reopen this decision.

## Consequences

**Easier**: the go/no-go question that gated WP-6 is resolved with field evidence rather than
argument — Level 3 is justified, not merely plausible. The gyro question has a concrete, falsifiable
trigger ("does the accel-only tilt estimate lag in the field") instead of being an open design
question revisited from scratch each time it comes up.

**Harder**: Level 3 cannot be built next — it still requires Level 2 (WP-5, 3-axis calibration) first,
per the existing dependency chain, even though the go decision itself is now certain. The project does
not get to skip straight to the highest-value fix.

**Gave up**: the cheaper static-offset correction, which would have needed no new calibration step and
no accelerometer fusion at all — ruled out on the data, not on suspicion, since a per-heading table
would in the limit become "recompute the heading correctly," i.e. tilt compensation, anyway.
