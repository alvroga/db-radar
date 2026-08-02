# Compass↔Accelerometer Frame Rotation — Bench Procedure (WP-6)

**Status**: Data collected and analyzed 2026-08-02 (`tiltbench_001.csv`, 2 passes × 6 poses) — see
Results below. Frame rotation resolved, formula implemented and field-confirmed:
`docs/compass_calibration_foundation.md` WP-6,
[ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md).

## Why this needs new data

WP-6 (tilt compensation, [`compass_calibration_foundation.md`](compass_calibration_foundation.md)
§6.2 item 3, §10) needs the fixed rotation between the magnetometer's sensor frame and the
accelerometer's sensor frame. The two chips are on **different PCBs** — the QMC5883L on the BH-880
module (cable-mounted), the QMI8658 on the Waveshare main board — so their axis correspondence is a
hardware fact, not something derivable from either datasheet or from the mag-only field data already
collected (WP-2/WP-3).

The mag-vs-**device** sense is already known (§3.3: pitching the device pins mag X toward "up"), but
that was established purely from how the on-screen N-indicator moved — it says nothing about which
*accelerometer* axis corresponds to which physical direction, because the accelerometer was never read
until WP-1. This is a genuinely new unknown, flagged as such in §10: *"needs a dedicated bench
procedure, not a field sample."*

## What's already known for free

A flat 360° spin cannot calibrate mag Z (§3.4) precisely *because* the Z axis stays pointed at the
same part of the sky throughout — i.e. **mag Z already points along the device's vertical axis when
flat**, the same physical direction as whichever accelerometer axis reads ≈±1 g in the FLAT pose. So
the FLAT pose alone fixes the mag-Z↔accel-axis correspondence (and its sign, read directly off the
printed numbers). What's still unknown is the correspondence for the two *horizontal* axes — which
accel axis is mag X, which is mag Y, and with what sign — which needs two more physical poses.

## Tool: Settings > DEV > Tilt Bench

⚠️ **Not a serial-monitor procedure.** Serial requires USB, and USB makes the NOSE-UP/DOWN and
EDGE-DOWN poses awkward to hold cleanly with a cable attached — and a cable/charger is itself a
plausible local magnetic-field source right next to the magnetometer, exactly the kind of
contamination this whole calibration effort exists to avoid (same reasoning as WP-1's field_log,
§8.1). This procedure uses a dedicated on-device screen instead, on battery, with results going to a
CSV file retrieved afterward over WiFi — same retrieval path as field_log's `/logs` page.

**Settings > DEV > Tilt Bench**:

- **Start Session** — opens `/sdcard/logs/tiltbench_<NNN>.csv` on the SD card.
- **Pose selector** (top button) — shows the current pose; auto-advances after each capture, or tap
  it to redo/skip a pose manually.
- **CAPTURE** — one-shot accel+mag readout, written as one CSV row tagged with the current pose.
  Chirps to confirm (you may not be looking at the screen in several of these poses). A distinct rapid
  buzzer pattern means the capture failed (sensor not ready) — redo that pose.
- **End Session** — flushes and closes the file.
- **Back** — returns to Settings without needing to end the session first.

There is also a `compass tiltbench` serial command with the same underlying read, for a quick sanity
check at a desk with USB already attached (e.g. confirming the accelerometer answers at all before
walking away from the computer) — but it is not the tool for actually running the pose protocol below.

## Procedure

Do this on battery, away from magnets, speakers, metal desks/laptops, and other electronics. Hold
each pose still for ~1 s before tapping CAPTURE (let the accel settle) — each capture is single-shot,
not averaged. Rotate the device about **its own axes** between poses (don't spin/yaw it) so poses 2–5
stay comparable; this isn't required for correctness but makes the numbers easier to reason about by
eye.

| # | Pose | How |
|---|---|---|
| 1 | **FLAT** | Screen facing straight up, device level on the table. Reference pose. |
| 2 | **NOSE-UP** | Pitch 90° so the top edge of the screen points straight up (device vertical, screen facing you). |
| 3 | **NOSE-DOWN** | Opposite of #2 — pitch 90° the other way. Confirms the sign found in #2. |
| 4 | **LEFT-EDGE-DOWN** | Roll 90° so the screen's left edge points at the floor. |
| 5 | **RIGHT-EDGE-DOWN** | Opposite of #4. Confirms the sign found in #4. |
| 6 | **FLAT (repeat)** | Same as #1 — a drift/repeatability check. |

The pose selector cycles exactly these six in order and auto-advances after each CAPTURE, so the
normal flow is: Start Session → hold pose 1, CAPTURE → hold pose 2, CAPTURE → ... → hold pose 6,
CAPTURE → End Session → Back → join WiFi → download `tiltbench_NNN.csv` from `/logs`.

## What happens with the results

Download the CSV and share its contents (or paste the six rows). From them:

- Pose 1 (and 6, as a check) fixes mag-Z↔accel-axis correspondence and sign directly.
- Poses 2 vs 3 identify which accel axis becomes vertical under pitch, and which mag component moves
  into the vertical-sized range in step with it (same physical direction ⇒ that pair maps together;
  the sign flips consistently between #2 and #3 or the pairing is wrong).
- Poses 4 vs 5 do the same for roll, pinning down the last axis pair.
- The result is a signed permutation matrix (each mag axis ↔ ±1 accel axis) — consistent with both
  chips being mounted axis-aligned to their own rigid PCBs, not at an arbitrary continuous angle. If
  the data doesn't resolve into a clean signed permutation, that itself is informative (a genuinely
  non-axis-aligned mount) and changes the implementation approach.

Once resolved, this feeds the standard tilt-compensated heading formula (gravity vector from a τ-EMA
on accel, mag rotated into the accel/device frame via this matrix, project onto the horizontal plane,
`atan2`) — signs and axis conventions fixed on hardware, not on paper, per the project's established
method for this whole line of work (§3.3, the WMM declination sign, the compass mounting check).

**Not a substitute for a field sample**: this only fixes the frame rotation. The gravity-estimate τ
and the standard formula's own sign conventions still get verified on a walk afterward, same as every
other constant in this document.

## Results (2026-08-02)

`tiltbench_001.csv` (also archived at `docs/calibration/tiltbench_001.csv`) resolved into a clean
signed permutation, not a non-axis-aligned mount: `device_x = cy, device_y = cx, device_z = cz`, no
extra negation.

The formula that ended up being implemented is **not** the textbook roll/pitch decomposition floated
above — that gave ~69° circular std against this data because its axis labeling didn't match this
hardware's actual mounting. A coordinate-convention-agnostic vector formula (gravity → "down", cross
with the device Y axis for "east/north", project mag onto that basis, `atan2`) fit the FLAT and
EDGE-DOWN poses to ~4° across both passes instead.

**NOSE-UP/NOSE-DOWN poses did not fit that formula's own self-consistency check** — not a data
problem. Those two poses are exactly where the device Y reference axis (used to build "east/north"
above) is parallel to gravity, a genuine singularity of this particular formula. They still served
their original purpose in this protocol: pinning down the mag-X↔accel-Y pairing and its sign directly
from the raw sensor values (§ above), independent of any downstream heading formula. `computeHeading()`
detects this condition and reports invalid rather than returning a garbage angle.

Full writeup, including why the first-principles sign guess was rejected in favor of the bench data:
[ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md).
