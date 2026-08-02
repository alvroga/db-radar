# Field trip data — 2026-08-01/02 (WP-2)

Raw CSVs from the §8.3 field trip in `docs/compass_calibration_foundation.md`. This file captures
context that is **not recoverable from the filenames or CSV headers alone** — walking direction,
in-field mislabeling, and discard flags the operator noted verbally right after the trip. Without this
file that metadata would be lost before WP-3 (offline analysis) ever reads these CSVs.

Numbering is monotonic across the whole SD card (`field_log::begin()` scans the directory), so it
starts at 006 here — 001–005 were consumed by the WP-1 pre-flight acceptance test
("start/stop 5 times in a row"), not trip data.

## Sample log

| # | File | RTC time | Rows | Label (actual) | Planned §8.3 # | Direction | Notes |
|---|---|---|---|---|---|---|---|
| 006 | `cal_006_flat360.csv` | 23:48:09 | 487 | flat360 | 1 | walking N | |
| 007 | `cal_007_phone360.csv` | 23:49:07 | 383 | phone360 | 2 | walking N | |
| 008 | `cal_008_phone360-actually flat360.csv` | 23:50:10 | 378 | **flat360** | 3 (repeat) | walking N | Operator pressed the wrong label on the selector — device was held flat, not phone-style. Filename already carries the correction; treat as the flat360 repeat (planned #3), not phone360 |
| 009 | `cal_009_walk-straight flat.csv` | 23:51:06 | 905 | walk-straight, flat | 5 | walking N | Control sample — isolates tilt from other error sources |
| 010 | `cal_010_walk-straight phone.csv` | 23:52:58 | 889 | walk-straight, phone-style | 4 | walking N | Headline tilt-bias sample, pairs with 009 |
| 011 | `cal_011_stand-still.csv` | 23:54:47 | 631 | stand-still | 6 | walking N | |
| 012 | `cal_012_disturbance.csv` | 23:56:09 | 335 | disturbance | 7 | walking N | |
| 013 | `cal_013_freeform.csv` | 23:57:08 | 606 | freeform | 8 | — (tumbled in place) | |
| 014 | `cal_014_disturbance.csv` | 23:58:35 | 551 | disturbance | 7 (repeat) | walking S | Second disturbance pass, opposite leg of the walk |
| 015 | `cal_015_shake-100hz.csv` | 23:59:40 | 1740 | shake-100hz | 9 | walking S | ~19 s at the dedicated 100 Hz task rate |
| 016 | `cal_016_shake-100hz.csv` | 23:59:59 | 5134 | shake-100hz | 9 (repeat) | walking S | Much longer than 015 — check duration/rate in WP-3 before treating it as a clean repeat |
| 017 | `cal_017_flat360-rotate left.csv` | 00:01:47 | 201 | flat360, rotate left | not in §8.3 | walking S | Extra variant, not in the original protocol — see below |
| 018 | `cal_018_flat360-rotate right.csv` | 00:02:07 | 177 | flat360, rotate right | not in §8.3 | walking S | Paired with 017 |
| 019 | `cal_019_phone360-rotate left.csv` | 00:02:25 | 204 | phone360, rotate left | not in §8.3 | walking S | Extra variant, not in the original protocol |
| 020 | `cal_020_phone360-rotate right.csv` | 00:02:45 | 170 | phone360, rotate right | not in §8.3 | walking S | Paired with 019, done immediately after |
| 021 | `cal_021_walk-straight -discard.csv` | 00:03:15 | 304 | walk-straight, **flat** (recovered) | 5 | walking S | Flagged for discard in the field (unsure of hold) — recovered via WP-3: `az≈−7647` matches the flat-hold signature from 009. See `wp3_results.md` |
| 022 | `cal_022_walk-straight-discard.csv` | 00:03:46 | 460 | walk-straight, **phone-style** (recovered) | 4 | walking S | Flagged for discard in the field — recovered via WP-3: `az≈−5379, ay≈−5127` matches the phone-style signature from 010 |
| 023 | `cal_023_stand-still.csv` | 00:04:46 | 623 | stand-still | 6 (repeat) | — (stationary) | End-of-session noise-floor check |

Row counts exclude the 3-line header (`# cc-radar field sample`, `# firmware=...`, `# rtc=...`).

## Deviations from the planned §8.3 protocol

- **Samples 17–20 (`rotate left` / `rotate right`) are not in §8.3.** The operator added them in the
  field to capture whether CW vs CCW rotation direction during the 360° sweep affects the circle-fit
  (hard-iron ripple) or the axis ratio. Useful extra data — just note in WP-3 that there's no
  "planned #" to cross-reference them against in §10's open-questions table; they support the axis
  ratio / stale-calibration-ripple questions, not a numbered planned sample.
- **Sample 8 was mislabeled at capture time**, corrected in the filename (see table). No CSV content
  is wrong — only the on-device button press was wrong, and the filename already reflects the true
  condition (flat, not phone-style).
- **Samples 21 and 22 were flagged as discard candidates** by the operator (unsure of hold
  orientation), but WP-3 resolved it from the accelerometer columns (`az`/`ay` split) instead of
  discarding them — 021 matches the flat-hold signature, 022 the phone-style signature. See
  [`wp3_results.md`](wp3_results.md).

## Direction annotation

Samples 006–012 were recorded while walking north; 014–022 while walking the return leg south
(013 freeform and 023 stand-still were stationary, no direction applies). This matters for samples
009/010 vs 021/022 specifically — the walk-straight samples are not just repeats of each other, they're
opposite-direction legs of the same out-and-back walk, which is relevant if tilt bias or GPS course
error turns out to be direction-dependent in WP-3.
