# WP-3 — Analysis results (2026-08-02)

Computed from the 18 CSVs in this directory (`cal_006`–`cal_023`) using [`analyze.py`](analyze.py)
(pandas/numpy only — `python3 analyze.py` from this directory, edit `DIR` at the top if run elsewhere).
Answers §10 of `docs/compass_calibration_foundation.md`; see that file's §10 table for the
one-line-per-question version.

## Verdict: data is good, no need to go again

- **Zero dropped rows in all 18 files** (`rows_queued == rows_written` everywhere) — the ring
  buffer/writer pipeline built in WP-1 held up completely in the field.
- **10 Hz sampling is metronomic**: `ms` deltas have σ ≈ 0.11 ms across the board.
- **100 Hz mode (samples 15/16) is equally solid**: σ ≈ 0.03–0.07 ms, and I2C failures during the
  recording were 0 and 1 respectively out of several thousand ops — the dedicated high-rate task does
  not stress the bus.
- **Freeform (013) swept elevation −87.6° to +82.5° and azimuth the full −180° to +180°** — near
  total sphere coverage in one 60 s tumble.
- The only real data-quality caveat: samples 017–020 (the rotate-left/right add-ons) are short
  (17–24 s, ~150–200 points) and likely don't cover a full 360°, so their circle-fit numbers below are
  less trustworthy than 006/008's. Not worth a retake — 006/008 already anchor the flat360 baseline.
- **021/022 don't need to be discarded.** The accelerometer's `az`/`ay` split cleanly identifies which
  was held flat and which was phone-style (see below) — reclassified rather than dropped.

No second trip needed. One nice-to-have, not required for any WP-6 decision: more phone-style
walk-straight samples in other compass directions (E/W) would fill out the heading-dependence found
below, but the go/no-go call doesn't need it.

## Headline finding: tilt error is heading-dependent, not a fixed bias

| Sample | Hold | Direction | Est. tilt from flat | heading_true − GPS course |
|---|---|---|---|---|
| 009 | flat | walking N | 6.4° | −0.06° ± 10.71° |
| 010 | phone-style | walking N | 49.7° | **−135.31° ± 29.35°** |
| 021 | flat *(reclassified via accel)* | walking S | 10.8° | +7.44° ± 16.48° |
| 022 | phone-style *(reclassified via accel)* | walking S | 45.4° | +4.11° ± 30.94° |

Reclassification: 021's `az ≈ −7647` matches 009's `az ≈ −7700` (device flat, gravity on Z); 022's
`az ≈ −5379, ay ≈ −5127` matches 010's `az ≈ −4864, ay ≈ −5624` (gravity split across Y/Z — tilted).
Filenames said "discard, unsure of hold" — accelerometer data resolves it outright.

This is the important part: **two samples at nearly the same tilt angle (49.7° vs 45.4°) produced a
135° error walking N and a 4° error walking S.** That is not noise — it's exactly what a 2-axis
compass (`atan2(cy, cx)`, no tilt compensation) is expected to do. At LA's ~59.7° magnetic
inclination, tilting the device rotates a large vertical field component into the horizontal
measurement plane, and *how much* depends on the interaction between the tilt axis and the local field
vector — which itself depends on which way the device is pointed. A fixed correction offset cannot fix
this; it moves. This is the go/no-go signal §5 and WP-6 were waiting on: **go** — real tilt
compensation (roll/pitch from the accelerometer feeding the heading formula, not a lookup table) is
required for phone-style holding to ever be reliable. Flat holding stays trustworthy in every
direction tested (both flat samples: mean error under 8°, std 11–16°) — that remains the safe fallback
in the meantime.

## `H₀` and the tilt (magnitude) threshold

| Sample | Hold | `h_mag` mean | resid (circle fit) | axis ratio |
|---|---|---|---|---|
| 006 | flat360 | 2993.2 | 4.36% | 1.062 |
| 008 | flat360 (relabeled from phone360, see `docs/calibration/README.md`) | 3024.7 | 2.18% | 1.066 |
| 017 | flat360, rotate left *(short/partial arc)* | 2721.4 | 3.63% | 1.172 |
| 018 | flat360, rotate right *(short/partial arc)* | 2749.5 | 2.71% | 1.065 |
| 007 | phone360 | 4112.7 | 20.07% | 1.919 |
| 019 | phone360, rotate left *(short/partial arc)* | 3387.2 | 11.10% | 1.519 |
| 020 | phone360, rotate right *(short/partial arc)* | 3589.6 | 15.39% | 1.675 |

- **`H₀` ≈ 3000 (raw LSB units)**, consistent between 006 and 008 to within ~1% — good repeatability.
- **Ripple/residual threshold**: flat360 sits at 2–4% circle-fit residual and axis ratio ~1.06–1.07 —
  that's the "healthy calibration" band. This must be measured on data already known to be flat (or
  gated by the accelerometer); it's not usable standalone as a tilt detector.
- **Tilt inflates `h_mag`, it doesn't reduce it** — phone360 reads ~23% *higher* than flat360
  (3696 vs 3009 mean), and the circle degrades from a near-circle to a fat ellipse (axis ratio up to
  1.9, residual up to 20%). This is the vertical field component leaking into `cx`/`cy` as tilt grows
  — exactly the same effect that produces the heading error above, seen from a different angle (pun
  intended). **`|H − H₀|/H₀ ≈ 0.23` at ~45–50° tilt is the number to use for a tilt/"not flat"
  detector**, keeping in mind the sign is "higher than H₀," not lower.

## Noise floor (stand-still, samples 011 and 023)

| Sample | `h_mag` std | heading circular std |
|---|---|---|
| 011 | 100.4 | 2.54° |
| 023 | 70.0 | 1.45° |

(011's naive `max−min` heading range reads as 359.7° — that's a wraparound artifact of taking a plain
range on a circular quantity that happened to sit near the 0°/360° boundary; circular std is the
number to trust.) **Noise floor ≈ 1.5–2.5° heading, ~2.5–3.3% relative in `h_mag`** — usable directly
for the EMA/deadband floor.

## Freeform (013) — 3-D calibration feasibility

Elevation swept −87.6° to +82.5°, azimuth the full −180° to +180° — a single ~60 s tumble covers
essentially the whole sphere. **Feasible**: a figure-8/tumble motion on this hardware does produce the
3-D coverage WP-5's ellipsoid fit would need. (The raw 3-D magnitude has 24.7% CV, but that's expected
and not a red flag — `compass_cal_z = 0` in the header, so Z is still uncalibrated; WP-5 is exactly the
step that fits and removes that.)

## Shake spectrum (samples 015/016, 100 Hz) and the accel-only-vs-gyro call

Both samples: clean 100.0–100.1 Hz actual rate, dt σ ≈ 0.03–0.07 ms, 0–1 I2C failures during the
recording (out of thousands of ops).

Accel-magnitude spectral energy (Hanning-windowed FFT), both samples agree closely:

| Band | 015 | 016 |
|---|---|---|
| 0.1–1 Hz | 2.3% | 2.4% |
| 1–3 Hz | 36.0% | 35.0% |
| 3–5 Hz | 22.1% | 20.6% |
| 5–10 Hz | 24.0% | 26.9% |
| 10–20 Hz | 9.5% | 9.4% |
| 20–50 Hz | 6.0% | 5.6% |

Clear peaks at **~1.95 Hz and ~3.9 Hz** — walking-cadence fundamental and its second harmonic, exactly
as expected for footstep-driven body shake. **~40% of the energy sits above 5 Hz** (a naive 10 Hz
sample's Nyquist limit), so sampling the accelerometer at a flat 10 Hz would alias a substantial
fraction of the shake spectrum back into the low-frequency band the gravity/tilt estimate actually
uses — corrupting it, not just losing detail.

**Answer to §6A.2**: accel-only suffices for tilt compensation — nothing here points at needing a
gyro. But it must be **oversampled and averaged down**, not naively read at 10 Hz: e.g. sample at
50–100 Hz internally and feed a τ-based EMA for the gravity estimate, so the 1–4 Hz walking energy
gets smoothed out rather than aliased in.

## Heading spectrum, flat walk (009) — body shake barely reaches the heading signal when flat

Detrended heading residual std: **4.81°** (vs. 10.71° when compared against GPS course — GPS course
itself is noisier than the compass here). Dominant frequency components are all below 0.2 Hz (5+
second periods) — nothing near the 2 Hz walking cadence found in the accel spectrum above. **When held
flat, body shake does not meaningfully leak into heading.** This is good news for WP-7: it suggests the
existing `HEADING_SMOOTHING` (α = 0.15, τ ≈ 0.62 s) is already doing its job for the flat-hold case,
and the τ-per-zoom table is refining responsiveness-vs-smoothness, not fixing a shake-noise problem
that turned out not to exist in this data.

## Rotate-left vs rotate-right sanity check

| | resid% left | resid% right | axis ratio left | axis ratio right |
|---|---|---|---|---|
| flat360 (017/018) | 3.63% | 2.71% | 1.172 | 1.065 |
| phone360 (019/020) | 11.10% | 15.39% | 1.519 | 1.675 |

No systematic left-vs-right effect beyond what's explained by these being short, partial-arc fits —
rules out a rotation-direction artifact in the circle fit.
