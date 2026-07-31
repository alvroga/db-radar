# Beacon Direction Finding — design

**Status**: Proposed, not implemented. **Unblocked** as of 2026-07-31 — `docs/performance_optimization_backlog.md`
§7.3a (continuous passive scan) is built and **verified live at 4.24–4.37 Hz** (was 2.0 Hz), which
lands in the "marginal; workable outdoors" row of the table in §3 below. Nothing in this document has
been built yet; it remains a design.
**Date**: 2026-07-31

The question this answers: *standing still and rotating in place, can the device tell you which way to
start walking toward the beacon?*

**Yes — but not the way people assume, and not at today's BLE sample rate.**

---

## 1. What is impossible here, stated plainly

**Bluetooth 5.1 AoA/AoD direction finding cannot be done on this hardware.** It requires:

- an **antenna array** with an RF switch (we have one onboard antenna, and no free GPIO for a switch —
  see `memory/hardware_constraints.md`),
- access to **IQ samples** of the Constant Tone Extension, which the ESP32-S3 radio does not produce
  and ESP-IDF's NimBLE does not expose.

No amount of software makes a single-antenna radio measure phase difference. Do not go looking for a
library that claims otherwise.

## 2. What is possible: body-shadow direction finding

The technique the user actually described — *stand still, rotate* — is not AoA. It is **body-shadow
DF**, an established manual radio-direction-finding method.

The human body is largely water and attenuates 2.4 GHz by roughly **10–20 dB**. Held at chest height
and rotated through 360°, the device sees a clear path when the beacon is in front of you and a
torso-shadowed path when you have turned away. That is a large, systematic directional signature — 
large compared to the ~±4 dB of multipath noise, **provided enough samples are averaged per
direction**.

The reason this is not a common phone feature is that it needs the heading for *every RSSI sample*.
**We have a QMC5883L magnetometer at 10 Hz.** That is the entire enabler, and it already exists for
the radar's heading-up rotation.

## 3. Why it has not worked so far — this is a sample-rate problem

Bin a 360° rotation into 12 sectors of 30° over a 10-second turn:

| BLE sample rate | samples/rotation | per 30° bin | verdict |
|---|---|---|---|
| 2.0 Hz — pre-§7.3a (backlog §7.1) | 20 | 1.7 | unusable — noise exceeds signal |
| **~4.3 Hz — confirmed live, §7.3a, tag at 200 ms** | ~43 | **~3.6** | marginal; workable outdoors |
| 10 Hz — §7.3a, tag reconfigured to 100 ms | 100 | **8.3** | **works** — not yet tried |

At 8 samples per bin, averaging reduces RSSI noise by `√8 ≈ 2.8×` — ~4 dB raw becomes ~1.4 dB per
bin, against a 10–20 dB body-shadow signal. **7–14 dB of SNR on the directional feature.** A slower
20-second rotation doubles it again. At ~3.6 samples/bin (today's confirmed rate) the noise reduction
is only `√3.6 ≈ 1.9×` — workable but noticeably weaker; a 20-second rotation (~7 samples/bin) is the
practical way to make today's rate usable without touching the tag.

At 1.7 samples per bin there is nothing to average and the answer is noise. That was the situation
before §7.3a; it no longer is.

> **This feature was blocked on data rate, not cleverness — and the blocker is cleared.** Backlog
> §7.3a (continuous passive scan) is built and confirmed at ~4.3 Hz. It is enough to *attempt* the
> feature (workable outdoors, per the table above), but reconfiguring the tag to 100 ms remains
> worthwhile before or during implementation — it roughly doubles per-bin sample count and halves
> trend latency elsewhere in the system too.

## 4. Algorithm

### 4.1 Pairing heading to RSSI — no synchronisation machinery needed

The System Task publishes `latest_heading` to a global; the NimBLE `onResult` callback reads it when
a packet arrives. Worst-case staleness is one System Task tick, **100 ms**. At a 10-second rotation
(36°/s) that is **3.6° of error** against 30° bins — negligible. No queue, no timestamp matching, no
mutex.

### 4.2 Bearing estimate: first circular harmonic, not `argmax`

Do **not** take the peak bin. It discards most of the samples and latches onto whichever bin caught a
lucky reflection. Fit the first harmonic instead — a two-accumulator DFT, updated per packet:

```c
// per advertisement packet
X += rssi_i * cosf(heading_rad_i);
Y += rssi_i * sinf(heading_rad_i);
W += fabsf(rssi_i);
n += 1;

// on request
bearing_rad = atan2f(Y, X);
confidence  = sqrtf(X*X + Y*Y) / W;   // circular resultant length, 0..1
```

This uses every sample and is robust to a single bad bin. Per-bin means are still worth keeping for
a **coverage check** — refuse to answer until every sector has ≥ K samples, or the estimate is biased
toward wherever the user lingered.

### 4.3 The confidence gate is the safety-critical part

`confidence` is what makes this shippable. When the RSSI pattern is flat — beacon too close, or
indoor multipath smearing the lobe — the resultant collapses toward zero. **The device must then say
"no direction — walk 15 m and retry", not point somewhere.**

The failure mode of every DF system is lying with conviction. Pick the threshold empirically and err
toward refusing.

## 5. Unknowns that must be measured, not derived

**⚠️ The sign of the peak is unknown.** If body shadowing dominates, peak RSSI = beacon direction. But
the device's own radiation pattern is asymmetric (PCB ground plane, LCD, battery), and it may offset
or even invert the peak. **Calibrate empirically**: place the beacon at a known bearing, rotate, and
record where the peak lands. Repeat at 10 m, 25 m and 40 m.

This project's own record is the reason to insist on this — see "The residual trap" and the §6
effort-vs-impact table in the performance backlog. Reasoning about RF from first principles has a
worse track record here than measuring it once.

## 6. Operating envelope — be honest about it in the UI

| Condition | Behaviour |
|---|---|
| Outdoors, 10–40 m | **Works.** Expect ±30–45°, i.e. reliable quadrant |
| Indoors | **Unreliable** — wall reflections make real-amplitude false peaks. Confidence gate should refuse |
| Under ~5 m | Degenerates — signal saturates, near-field effects dominate. Not needed: the sonar tempo already tells you |
| Beyond ~40 m | Too few packets, signal near the noise floor |

**±30–45° is not "point at the beacon". It is "start walking that way"** — which is exactly the
question asked.

Note the useful band (10–40 m) is precisely where the current 4-step sonar tempo conveys almost
nothing (backlog §8.1e). The two features cover each other's weak zone.

**Tilt**: QMC5883L heading is only valid held level; there is no tilt compensation. Error is a few
degrees against 30° bins — ignorable initially. The QMI8658 IMU is physically on the board with a
vendor driver present but disabled for I2C contention (`src/hardware/sensors/gyro_qmi8658.cpp`); it is
the known upgrade path if tilt proves to matter.

## 7. The complement: GPS gradient DF

A second, independent technique that is arguably **more robust outdoors and needs no user ritual**.

Log `(lat, lon, rssi)` passively as the user walks. Two legs of ~15 m give a bearing by trilateration
against **real GPS baselines** rather than body-attenuation statistics. It accumulates in the
background — no special mode, no standing still, no rotating.

- **Rotation DF** answers *"I'm standing still and lost."*
- **Gradient DF** answers *"I've been walking — am I converging?"*

They are complementary. Gradient DF is the easier and more reliable of the two and could be built
first; it shares the same §7.3a sample-rate dependency but is far less sensitive to it, since GPS
baselines carry the geometry instead of RSSI shape.

## 8. Proposed UX sketch (not decided)

1. Available only at 50 m zoom, where beacon proximity is already active.
2. User triggers "find direction" (long-press, or a control on the beacon screen).
3. Prompt: *"Hold flat at chest height and turn slowly all the way around."*
4. Live coverage arc fills as sectors are collected; a countdown of remaining sectors.
5. On sufficient coverage **and** confidence: draw a bearing arrow on the radar, rotating with the
   compass like the existing N indicator.
6. On low confidence: *"No clear direction — walk 15 m and try again."*

## 9. Dependencies and order

| # | Item | Where |
|---|---|---|
| 1 | ~~Continuous passive BLE scan — 2 Hz → 5/10 Hz~~ | ✅ **done**, backlog §7.3a, confirmed ~4.3 Hz |
| 2 | Set the tag's advertising interval to 100 ms | hardware config — **not yet done**, worth doing before/during build |
| 3 | Publish `latest_heading` global from System Task | `task_manager.cpp` |
| 4 | Accumulator + confidence in a new DF module | new file |
| 5 | **Empirical sign/offset calibration** | hardware task, §5 above |
| 6 | UI mode + bearing arrow | `navigation.cpp`, `ui_manager.cpp` |

**Item 1 is done; item 2 is optional-but-recommended, not a hard blocker anymore.** At the confirmed
~4.3 Hz the result is workable-outdoors per §3's table, not random-number-generator noise as it was at
2 Hz — but it is materially better with 100 ms advertising (per-bin sample count roughly doubles), so
doing it either before starting or in parallel with items 3–4 is the efficient order.

---

**Related**: [`performance_optimization_backlog.md`](performance_optimization_backlog.md) §7 (BLE rate),
§8.1e (sonar tempo in the same distance band) · [`beacon_proximity.md`](beacon_proximity.md) ·
[`navigation_modes.md`](navigation_modes.md) (heading source, N indicator rotation)
