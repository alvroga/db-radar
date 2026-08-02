# Compass Calibration — Foundation and Field Data Plan

**Status**: Proposal — ready to implement from | **Date**: 2026-08-01

This document lays the foundation for compass calibration work. It states what calibration means on
this hardware, what we have today, what each further level costs and delivers, the field data
collection that has to happen first — because most of the constants involved cannot be guessed — and
the ordered implementation plan that follows from it.

**If you are starting a fresh session on this: go to §13 first**, then §12. Sections 1–11 are the
reasoning those two rest on.

Decisions here that have real alternatives get ADRs of their own when built; candidates are listed in
§13.

---

## 1. The problem this comes from

Field experience, stated plainly by the user:

> While walking toward a waypoint there is a lot of body motion, and the device is normally held
> **phone-style, facing you** — at that angle the compass is unreliable. As you get closer you stop
> more often and need more precision, and that's where the compass is unparalleled — but that
> requires holding the device **parallel to the ground**.

That is an accurate description of an uncompensated 2-axis magnetometer, and the error is larger than
it feels (§3.2). Three distinct gaps follow from it:

1. **Heading is only valid held flat.** A phone works at any angle; ours does not.
2. **Nothing tells us the calibration has gone stale.** Opening the enclosure invalidates it
   (project memory: `compass_recal_after_case_open.md`) and the failure is silent — the compass keeps
   returning a confident, wrong number.
3. **Nothing distinguishes a bad calibration from a passing magnetic disturbance.** These need
   opposite user responses (recalibrate vs. step away) and currently produce identical symptoms.

---

## 2. What exists today

| Aspect | Current state | Reference |
|---|---|---|
| Heading formula | 2-axis `atan2f(cy, cx)` — no tilt compensation | `compass_qmc5883l.cpp:120` |
| Hard-iron correction | X and Y only | `compass_qmc5883l.cpp:113-114` |
| Z axis | Read into `z_raw`, offset **stored but never applied** | `compass_qmc5883l.cpp:106, 128-132` |
| Calibration procedure | 360° spin, **flat on a surface**, min/max per axis, offset = (max+min)/2 | `settings_screen.cpp:120-121, 161-190` |
| Z offset at save time | Hardcoded `0` | `settings_screen.cpp:186-187` |
| Calibration quality | Coverage span **plus** `H0`/circle-fit residual/axis ratio (WP-4) | `settings_screen.cpp` calibration overlay |
| Runtime health check | `compass_qmc5883l::classifyHealth()` — magnitude vs. `H0`, EMA+hysteresis (WP-4) | `compass_qmc5883l.h/.cpp`, `navigation.cpp` HUD label |
| Field magnitude | Computed implicitly, **discarded** | `compass_qmc5883l.cpp:117-120` |
| Declination | WMM2020 (n=1..3), once per session at first fix, NVS-cached | `wmm_declination.cpp`, `task_manager.cpp:1537-1541` |
| Sensor config | 2 G range, 200 Hz ODR, 512 OSR, continuous | `compass_qmc5883l.cpp:48-50` |
| Read rate | 10 Hz from System Task | `task_manager.cpp:1462-1473` |

### 2.1 Two documentation defects found while writing this

Both are comments contradicting working code. **The code is correct** — the sign was verified
empirically and is recorded in project memory (`feedback_wmm_sign.md`):

- `include/utils/wmm_declination.h` — *"Apply: true_heading = magnetic_heading - declination"*
- `include/settings_manager.h:69` — *"Apply: true_heading = magnetic_heading - compass_declination_deg"*

Actual code, `task_manager.cpp:1541`: `true_heading += decl_settings.compass_declination_deg;`

Fix the comments, do not "fix" the code.

---

## 3. Physics foundation

### 3.1 The field we are measuring

Earth's field is a vector, not a direction. In the Los Angeles area (≈34.1°N, 118.2°W, epoch 2026):

| Quantity | Symbol | Approx. value | In QMC5883L LSB @ 2 G |
|---|---|---|---|
| Total intensity | F | ~47 µT | ~5 640 |
| Horizontal component | H | ~24.5 µT | ~2 940 |
| Vertical component | Z | ~40 µT | ~4 800 |
| Inclination (dip) | I | ~58.5° below horizontal | — |
| Declination | D | ~12.2° East | — |

Scale: 2 G range → 12 000 LSB/Gauss → **120 LSB/µT**; full scale ≈ 273 µT, so there is ~5× headroom
over the Earth field. Saturation (the `overflow` flag) therefore means a strong local source, not
Earth.

**The key number is the inclination.** The field points mostly *down*, not north — the vertical
component is 1.6× larger than the horizontal one we actually want.

### 3.2 Why tilt hurts so much

`atan2(cy, cx)` assumes the sensor's XY plane is horizontal. Tilt the device and the large vertical
component leaks into X and Y. Worst-case heading error is approximately `atan(tan(I) · sin(θ))`:

| Tilt θ | Worst-case heading error (I = 58.5°) |
|---|---|
| 10° | ~16° |
| 20° | ~29° |
| 45° | ~49° |
| 60° (phone-style) | ~55° |

So ~10° of unnoticed tilt already costs more than a compass rose sector. This is a **bias**, not
noise: it does not average out, and no amount of smoothing removes it. Smoothing addresses body shake
(a separate, genuine problem — see §9.1), not this.

Tilt also changes the *measured horizontal magnitude*, which is what makes §5 possible. Facing north
and pitching by θ about the East axis, the sensor's X reads `H·cosθ ± Z·sinθ`. At θ = 20° that is
either ~36.7 µT or ~9.3 µT against a flat value of 24.5 µT — i.e. **+50% or −62%**. Near the
cancelling angle the horizontal signal nearly vanishes and the heading becomes meaningless, which is
exactly where a magnitude check screams loudest. That is the correct failure behaviour.

### 3.3 Field-confirmed: the vertical inversion, and the 31.5° cliff

Observed on a walk, 2026-08-01:

> Facing north with the radar **horizontal**, N points to the top of the screen — correct. Still
> facing north but holding the radar **vertical**, N goes to the **bottom** — the opposite direction.

This is exactly what §3.2 predicts, and it is worth working through because it is the cleanest
possible confirmation that the model is right and that nothing exotic is going on.

Take the sensor's X axis as pointing "forward" when the device is flat, and pitch the device up by θ:

```
mx = H·cosθ − Z·sinθ            (facing magnetic north)
my = 0
heading = atan2(my, mx)
```

- **θ = 0° (flat)**: `mx = +H = +24.5` → `atan2(0, +24.5)` = **0°**, N at top. ✅
- **θ = 90° (vertical)**: `mx = −Z = −40` → `atan2(0, −40)` = **180°**, N at bottom. ✅ — matches the
  field observation exactly.

**The crossover is at θ = atan(H/Z) = 90° − I ≈ 31.5°.** At that tilt `mx` passes through zero and the
computed heading swings through ±90° of error; beyond it the sign inverts and the compass reads
backwards. Geometrically it is simply the angle at which the sensor's X axis becomes perpendicular to
the field vector — and since the field is 58.5° below horizontal, that happens only 31.5° above it.

So the usable tilt budget facing north is **well under 31.5°**, and phone-style (~60°) is deep past it.
"Less reliable when tilted" understates it: past the cliff the device is confidently backwards.

Two things fall out of this for free:

**It pins down the mag↔device frame sense.** Getting exactly **180°** rather than 0° tells us `mx` goes
*negative* when the device is pitched up — i.e. the sensor X axis rotates toward **up**, not down. That
was listed as an unknown in §10; the field observation has now answered half of it (mag vs *device*).
The mag vs *accelerometer* rotation is still open, because the accelerometer has never been read.

**Held vertical, the heading is not merely wrong — it is nearly dead.** At θ = 90° with the device
turned to azimuth ψ, the X axis points up regardless of ψ, so `mx = −Z` is *constant* and only
`my = −H·sinψ` varies:

```
heading = atan2(−H·sinψ, −Z)     →    confined to 180° ± atan(H/Z) = 180° ± 31.5°
```

Turn a full 360° while holding it vertical and the displayed heading sweeps a **~63° arc centred on
south**, then returns. It barely responds to rotation at all. That is a sharper and more falsifiable
prediction than "the compass is unreliable", and sample 2 (`phone360`) will pin down its exact shape.

**Field-confirmed at four azimuths, 2026-08-01.** Method: stand at a fixed azimuth, note where the N
indicator sits with the radar **flat**, then — **without moving or turning** — tilt the radar to
**vertical** and note where N moves to. Each row is therefore a controlled pair with azimuth held
constant and tilt as the only variable:

| Facing (flat) | N when flat | N when vertical | Implied heading | Predicted |
|---|---|---|---|---|
| N | top (0°) | bottom | 180° | **180° exactly** |
| E | left (90°) | bottom | ~180° | 180° ∓ 31.5° |
| S | bottom (180°) | **no change** | 180° | **180° exactly** |
| W | right (270°) | bottom-right | ~225° | 180° ± 31.5° |

The **N and S rows are the load-bearing ones**, because the model predicts them as *exactly* 180° with
no free parameters: at ψ = 0° and ψ = 180°, `my = ±H·sinψ = 0`, so the heading is `atan2(0, −Z) = 180°`
regardless of H, Z, inclination, or calibration. Both matched — including the case where **facing south,
tilting the device changes nothing at all**, because the flat reading (180°) already equals the value
the tilt forces. A degenerate prediction coming out degenerate is the strongest confirmation available
without logged data.

E and W landed as the two off-centre extremes, as predicted, but with a lopsided spread (~0° and ~45°
from centre against a symmetric ±31.5°). Do not read anything into the asymmetry yet: these are
eyeballed indicator positions on a round screen, and near vertical the heading is extremely sensitive
to the exact tilt angle — a few degrees of posture difference between the two trials moves the
endpoints substantially. Because the azimuth was held fixed, *turning while tilting* is ruled out as an
explanation; posture and eyeball precision are what remain. The **sign of the excursion per azimuth is
still open** and is what sample 2 resolves quantitatively.

**The N/S result also rules out roll contamination**, which matters because "tilt to vertical" is
ambiguous between two motions. Pitching about the screen's left–right axis sends device **X** toward
the sky (`mx = −Z`, giving 180° at ψ = 0°). Rolling about the up–down axis would send **Y** there
instead, giving `atan2(±Z, H) = ±58.5°` at ψ = 0° — nowhere near the bottom of the screen. Getting
180° means the motion was pitch about the left–right axis and the sensor's X axis rotates toward *up*,
independently re-confirming the mag↔device frame sense.

**Consequence for Level 1**: confirmed as unambiguous. At vertical, `H_meas = sqrt(Z² + (H·sinψ)²)`
ranges 40–47 µT against `H₀ ≈ 24.5` at *every* azimuth tested — there is no orientation where the
device is held phone-style and the magnitude check stays quiet.

**And it confirms Level 1 will catch this case unambiguously.** At vertical,
`H_meas = sqrt(Z² + (H·sinψ)²)` ranges from 40 to 47 µT against `H₀ ≈ 24.5` — i.e. **1.6× to 1.9× the
baseline**. No threshold tuning can miss that. The exact holding posture that breaks the compass is
the one the magnitude check flags loudest.

### 3.4 What "calibration" actually covers

Three distinct error sources, commonly conflated:

- **Hard iron** — a *constant additive* offset from permanently magnetized material travelling with
  the sensor (battery, speaker magnet, screws, the enclosure). Shifts the measurement sphere off the
  origin. This is what the min/max procedure corrects, and it is the dominant error here.
- **Soft iron** — *multiplicative* distortion from ferrous material that concentrates the field,
  turning the sphere into an ellipsoid. Needs a 3×3 matrix, not an offset. Currently uncorrected and
  probably minor, but the calibration data will show whether it is (§5.3).
- **Mounting/frame** — the compass sits on the **BH-880 module**, a separate PCB on a cable, while
  the accelerometer sits on the **Waveshare main board** (`docs/bh880_module.md:264-265`). Their axis
  frames are unrelated until measured. Empirical checks found no mounting offset needed for the
  compass alone (`docs/compass.md:23-29`), but that says nothing about mag-vs-accel alignment, which
  §6 depends on entirely.

A **flat 360° spin cannot calibrate Z at all** — the Z axis stays pointed at the same part of the sky
the whole time, so min ≈ max and the offset is unrecoverable. That is why the current procedure
hardcodes `0`, and why any 3-axis use needs a different motion (§6.2). It is also why phones make you
do the figure-8.

---

## 4. Staged levels

Each level is independently useful and shippable. Later levels depend on earlier ones.

| Level | What | Needs | Delivers | New hardware |
|---|---|---|---|---|
| **0** | Today | — | Heading when held flat | — |
| **1** | Calibration **health metrics** | Nothing new — the data is already read and thrown away | Detects stale calibration, magnetic disturbance, and *that* the device is tilted | None |
| **2** | Full **3-axis calibration** | New calibration motion + coverage/fit scoring; apply `cal_z_offset` | A valid 3-D field vector — the prerequisite for L3 | None |
| **3** | **Tilt compensation** | Accelerometer (QMI8658, already on the board) + L2 | Heading correct at any angle — the iPhone behaviour | None (chip already present) |

**Level 1 is the priority regardless of whether 2 and 3 ever happen**: it costs no bus traffic, it is
valuable on its own, and its output is what tells us how big the tilt problem actually is — which is
what justifies (or doesn't) the cost of Level 3.

---

## 5. Level 1 — health metrics

### 5.1 The one number

`read()` already fetches all six data bytes in one burst and already applies the X/Y hard-iron
offsets. It computes `atan2(cy, cx)` and **discards the magnitude**:

```
H_meas = sqrt(cx² + cy²)
```

With a correct calibration and the device flat, **H_meas is constant regardless of heading** — it is
the horizontal field strength. Every failure mode perturbs it distinguishably:

| Observation | Meaning | Correct user action |
|---|---|---|
| `H ≈ H₀`, steady | Flat and healthy | none |
| `H` far from `H₀` (either direction), tracks handling, transient | **Device is tilted** | "hold it flat" |
| `H` varies **sinusoidally with heading**, persistent, modest | **Hard-iron calibration stale** | recalibrate |
| `H` erratic, spiking, correlates with location not heading | **Local magnetic disturbance** | step away — do *not* recalibrate |
| `overflow` flag set | Sensor saturation, very strong source | already handled |

The two main cases separate by **magnitude** (tilt swings H by tens of percent — §3.2; a stale
hard-iron offset produces a few to ~20%) and by **dynamics** (tilt correlates with handling, hard iron
is periodic in heading and persistent). The field data (§8) is what fixes the actual thresholds.

**This detects, it does not correct.** Recovering true heading from a tilted reading requires the tilt
*axis*, which the magnetometer alone cannot supply. Level 1's honest output is a trust indicator, not
a fix.

### 5.2 `H₀` is free

The existing calibration overlay already instructs *"Flat on a surface. Rotate slowly 2 full circles"*
and already records min/max on X and Y (`settings_screen.cpp:161-164`). So:

```
H₀ = ((max_x − min_x) + (max_y − min_y)) / 4      // mean semi-axis radius
```

is computable from data already collected, at zero extra cost, and stored alongside the existing
offsets in NVS. Everything in §5.1 is relative to a baseline we can capture today.

### 5.3 Two more free quality scores at calibration time

- **Axis ratio** `(max_x − min_x) / (max_y − min_y)`. Should be ≈ 1.0 for a circular locus. A
  persistent departure is **soft iron** or per-axis scale error, and quantifies whether §3.3's "probably
  minor" assumption holds.
- **Circle-fit residual** — RMS of `|H_sample − H₀|` across the sweep. This is the calibration's own
  goodness-of-fit, and it is the natural thing to show instead of the current coverage-only
  indicator. A calibration that "completed" with a poor residual is worse than no calibration, because
  it reads as trustworthy.

### 5.4 Optional absolute cross-check against WMM

`H₀` is self-referential — it detects *drift from your own baseline*, not absolute correctness. WMM
gives the absolute value, but `wmm_declination.cpp` currently computes only the horizontal X (north)
and Y (east) components and returns `atan2(Y, X)`; it never forms the radial Z component
(`wmm_declination.cpp:130, 149-150`).

Adding Z is roughly ten lines using the associated Legendre values already computed in the same
function, and yields F and I for free. Caveat: the n=1..3 truncation is specified at ±1° for
*declination*; **intensity accuracy at that truncation is not characterised** and is certainly worse.
Adequate for a ±20% sanity gate, not for fine calibration. Treat as a nice-to-have, not a
prerequisite — and verify the truncation error against a reference model before trusting it.

---

## 6. Level 3 — tilt compensation

Included here because it sets requirements on Level 2's design.

### 6.1 Accelerometer vs gyroscope — different sensors

Commonly bundled in one chip, which blurs them, but they measure different quantities:

| | Measures | Gives us | Fails at |
|---|---|---|---|
| **Accelerometer** | Proper acceleration (g); at rest, **gravity** | **Which way is down** → absolute tilt, never drifts | Cannot separate gravity from motion; walking adds ±0.3–0.5 g at ~2 Hz |
| **Gyroscope** | Angular **rate** (°/s) | How fast we are turning — smooth, immune to linear acceleration | Says nothing about "down"; needs integration, so it **drifts** |

They are complementary, not redundant. For tilt compensation the question is literally "where is
down", so the **accelerometer is the required one**; the gyro would only reject step-shake faster.

**They can be used independently.** The QMI8658 is a 6-axis IMU with separate enable bits (`CTRL7`),
separate rate/scale registers (`CTRL2` accel, `CTRL3` gyro) and separate data registers (accel at
0x35–0x3A) — `include/hardware/sensors/gyro_qmi8658.h:12-17, 35-38`. Enable the accelerometer, leave
the gyro powered down.

Note what was removed previously: `src/navigation/imu_sampling.cpp` did **gyro** heading fusion at
**100 Hz** (CHANGELOG.md:1059-1065). That is a different sensor for a different purpose at ten times
the rate. This proposal is not "bring the IMU back".

### 6.2 What it requires

1. **A valid 3-axis calibration** (Level 2) — including a real `cal_z_offset`, which needs a
   tumble/figure-8 motion with 3-D coverage scoring, and `read()` actually applying it.
2. **A gravity estimate.** Raw accelerometer while walking = gravity + step acceleration. Step
   acceleration is roughly periodic at ~2 Hz while device orientation changes much more slowly, so a
   τ ≈ 0.5–1 s EMA on the accel *vector* separates them. This is the project's existing
   filter-the-input pattern (`continuous_over_hysteresis.md`); the τ comes from the field data (§8).
3. **The mag-to-accel frame rotation.** The two sensors are on **different PCBs** (§3.4). The fixed
   rotation between their frames must be determined empirically, exactly as the declination sign and
   the compass mounting were. This is a real unknown and the most likely source of a
   confidently-wrong first implementation.
4. The standard tilt-compensated form (roll/pitch from gravity, project the mag vector onto
   horizontal, then `atan2`). Sign and axis conventions get fixed on hardware, not on paper.

---

## 6A. The gyroscope — deferred, not rejected

The gyro was discarded as a **heading source**, which is the one job it structurally cannot do: it
measures angular *rate*, so heading requires integration, and integration drifts. Against an absolute
sensor it loses on principle, and `imu_sampling.cpp` (100 Hz gyro fusion, CHANGELOG.md:1059-1065) lost
to the QMC5883L fairly.

That is not a verdict on the gyro — it is a verdict on using it *alone*. Its actual strength is
short-term rotation measurement alongside an absolute sensor that anchors it. Two jobs in this project
now match that shape:

**Use 1 — stabilise the gravity estimate (§6.2 item 2).** Optional. The accelerometer is the required
sensor; the gyro only helps reject step acceleration faster.

**Use 2 — heading shake, which is the more valuable one.** Confirmed in the field on 2026-08-01: *"since
the compass is so responsive it bounces non stop."* An EMA buys smoothness by paying lag — a strict
trade, and a τ-per-zoom table (§9.1) is just picking different points on it. A complementary filter
does not make that trade: it uses measured rotation rate to track a genuine turn immediately while
rejecting shake, because the two are physically distinguishable — a real turn is sustained rate, shake
is oscillatory and zero-mean. This is how phones manage to feel smooth and instant at once, and it is
the principled fix for the problem §9.1 works around.

### 6A.1 Cost — the bus is not the main one

**Adding the gyro to an accel read we are already doing is nearly free on the bus.** Accel data sits at
0x35–0x3A and gyro at 0x3B–0x40 — **contiguous** (`gyro_qmi8658.h:35-46`). Both is a single 12-byte
burst instead of a 6-byte one: ~+135 µs per read, about **+0.13 percentage points** at 10 Hz.

**But that is only true at 10 Hz, and at 10 Hz the gyro barely helps.** Step shake is ~2 Hz, so 10 Hz
gives roughly 5 samples per cycle — marginal for rejecting it. The previous implementation ran at
**100 Hz** for exactly this reason. At 100 Hz the bus cost rises tenfold *and* you are running a
100 Hz task against a bus with an undiagnosed timing floor (`I2C_PROCESS_MS`, ADR-0013) and an open
freeze issue (FT-06).

So the honest shape of the risk is not "it is on I2C" but: **the gyro is only useful at a rate where
being on I2C starts to matter.** The bus cost and the usefulness rise together.

Two costs that are not the bus at all, and may weigh more:

- **Power** — a gyro typically draws several times the accelerometer's current, on a battery device.
  Needs a datasheet check before anyone commits; do not take this on trust.
- **State that goes stale** — gyro bias needs its own calibration (the old code persisted `imu_bx/by/bz`
  in NVS) and drifts with temperature, and a complementary filter adds a tuning constant. That is this
  project's most frequently repeated defect: a constant nobody re-derives after the pipeline around it
  changes.

### 6A.2 How to decide it — sample 9

Logging raw accel at 10 Hz already yields the shake spectrum. The gyro rides along in the same burst
for effectively nothing, so log it at 10 Hz too, purely to see it.

The decisive test is **one dedicated high-rate sample** (§8.3 sample 9): 30 seconds of accel + gyro at
100 Hz while walking. That accepts the 100 Hz bus risk for half a minute in a controlled recording
rather than committing to it architecturally, and it is the only way to establish whether 100 Hz is
actually required or whether 10 Hz would do. Record `i2c_manager` failure statistics either side of it.

---

## 7. I2C bus risk assessment

### 7.1 Bandwidth — not the issue

Shared bus, SDA=GPIO15, SCL=GPIO7, 400 kHz. At 400 kHz one byte ≈ 22.5 µs including ACK; a register
read of N bytes ≈ (3 + N) bytes plus framing.

| Device | Addr | Rate | Approx. bus time/s |
|---|---|---|---|
| CST820 touch | 0x15 | 50 Hz (`TOUCH_POLL_INTERVAL_MS = 20`) | ~12 ms |
| QMC5883L compass | 0x0D | 10 Hz, 2 transactions (status + 6-byte burst) | ~3 ms |
| PCF85063 RTC | 0x51 | 0.2 Hz | negligible |
| TCA9554 EXIO | 0x20 | on demand | negligible |
| **QMI8658 accel (proposed)** | 0x6A/0x6B | 10 Hz, one 6-byte burst | **~2 ms** |

Total bus utilisation is on the order of **2%**, and the accelerometer adds roughly **0.2 percentage
points**. Bandwidth is not a meaningful constraint.

### 7.2 Address conflicts — none, and the chip is already on the bus

0x6A/0x6B collide with nothing in use (0x0D, 0x15, 0x20, 0x51). More importantly the QMI8658 is
**already physically present, powered, and ACKing** — it is in the i2c_manager known-device table
(`i2c_manager.cpp:303-304`) and in `system_config.h:290-291`, and it shows up in bus scans today.
There is no new capacitive load, no new pull-up burden, no new address. **Only transactions are
added.**

The existing vendor driver is also already on the safe path: `gyro_qmi8658.cpp` calls
`I2C_Read`/`I2C_Write` from `i2c_driver.h`, and that compatibility layer forwards straight to
`i2c_manager::read`/`write` (`I2C_Driver.cpp:10-19`) — so it inherits the bus mutex and the retry
logic rather than bypassing them. It simply has no callers and is not in the build. Its
`CTRL7 = 0x43` (`gyro_qmi8658.cpp:220`) enables accel **and** gyro; accel-only is `0x01`.

### 7.3 The real risks

Both are about latency and known-unstable behaviour, not bandwidth:

- **`I2C_PROCESS_MS = 20` is a tuned floor whose cause is undiagnosed.** Dropping it to 10 ms killed
  the button and the buzzer (ADR-0013, project memory `i2c_process_ms_floor.md`), and the documented
  explanation for *why* is known to be stale. The bus behaves in a way nobody has fully explained, so
  "the arithmetic says 0.2%" is necessary but not sufficient evidence.
- **FT-06 I2C freeze is open.** Its original root cause largely collapsed on 2026-07-31 (the
  61-device scan was a `diag i2c` probe bug), so the next freeze needs fresh evidence. Adding a sixth
  actively-read device changes the population under study, and could muddy that evidence.

### 7.4 Mitigations

- Read the accelerometer **in the same System Task tick, immediately after the compass**, inside a
  single mutex acquisition where the API allows — one lock/unlock, not two.
- Keep the **gyro disabled** (`CTRL7`), so the added traffic is one 6-byte burst.
- Put it behind a **runtime kill switch** (setting + serial command) so it can be disabled instantly
  in the field if the bus misbehaves, without a reflash.
- Follow the **existing compass suspension rules**: reads are fully suspended while the WiFi AP is up
  and re-initialised after standby (`task_manager.cpp:1465-1516`). The accelerometer must obey the
  same gates or it will reintroduce exactly the case those gates exist for.
- Record `i2c_manager` failure statistics before and after, and treat any change as blocking.

### 7.5 Other buses

Unaffected. GPS is UART (GPIO43/44), the display is RGB parallel + SPI init, SD shares SPI pins with
the LCD. The accelerometer touches none of them.

---

## 8. Field data collection

Most constants above cannot be guessed: `H₀`, the tilt threshold, the stale-calibration ripple
threshold, the accel EMA τ, the τ-per-zoom table, and the actual size of the tilt bias while walking.
They must be measured.

### 8.1 Hard constraint: no serial in the field

**The serial monitor requires USB, and USB also powers the 5 V rail and charges the battery.** A
battery-powered field trip cannot produce serial output — this is a standing project constraint. Any
plan of the form "walk around, then read the serial log" is impossible.

Logging must therefore write **files**, and the retrieval path already exists:

- `system_logger.cpp:171` already opens and appends to a file under `/sdcard/logs`.
- `gpx_server.cpp:873-879, 507-523` already serves a `/logs` page with a Download button and a
  `/download/logs/<file>` endpoint.

So: log to storage on battery in the field → come home, power up, join WiFi, download in a browser.
No new retrieval infrastructure.

**⚠️ Correction (found while building WP-1.4): this is the physical SD card, not FFat.** This
document previously said `/sdcard` was the FFat mount. It is not. `device_manager::initSD()` calls
`esp_vfs_fat_sdmmc_mount("/sdcard", ...)` against the SDMMC host — a real card in the slot — and the
11.7MB `ffat` partition declared in `partitions/partitions_ota.csv` is **never mounted by anything**
in the firmware. Consequences that matter for the trip:

- **A card must be physically inserted or there is nowhere to write.** `field_log::startSample()`
  fails with a clear reason and the screen shows "NO SD CARD", but that is a thing to discover at the
  desk, not at the trailhead.
- Capacity is the card's, so the ~11.7MB budget in §8.2 is moot — any card has room for all nine
  samples many times over. The auto-stop caps still matter (a forgotten session, not a full disk).
- Nothing else changes: the `/logs` page, the download endpoint, and `system_logger` all already
  target this same path, which is why the mistake was invisible until someone checked the mount call.

### 8.2 Session model — one file per sample

Per the user's requirement: a **start/stop button, producing a separate file per sample**, rather than
one long log to be cut up afterwards.

**Screen**: a DEV-mode "Field Log" screen.

- **START / STOP button** — large, reachable one-handed. START opens a new file; STOP closes and
  flushes it. The sample *is* the file, so there is no post-hoc segmentation step.
- **Label selector** — a short predefined list, advanced by a second button, so the label is baked
  into the filename rather than remembered later:
  `flat360`, `phone360`, `walk-straight`, `walk-glance`, `stand-still`, `disturbance`, `freeform`.
- **Filename**: `/logs/cal_<NNN>_<label>.csv`, `NNN` monotonic. RTC time in the header (the PCF85063
  is available) rather than in the name, to keep names short and sortable.
- **Audible confirmation** — a buzzer chirp on start and a double-chirp on stop. Non-negotiable: half
  these samples are taken while holding the device phone-style or tumbling it, i.e. while *not*
  looking at the screen. Silent start/stop will produce empty or doubled files.
- **Live readout while recording**: elapsed time, sample count, file size, FFat free space.
- **Auto-stop** on a time or size cap, so a forgotten session cannot fill the filesystem.

**Header** (one comment block per file, so a sample is self-describing when opened months later):
firmware version/build, RTC timestamp, active `compass_cal_x/y/z`, `compass_calibrated`, declination
and its validity, `H₀` if known, zoom level, and whether the accelerometer was enabled.

**Columns** (one row per System Task tick, 10 Hz):

```
ms, mx, my, mz, cx, cy, H, heading_mag, heading_true,
ax, ay, az,                       # zero/absent if accel not enabled
lat, lon, speed_kn, course, hdop, sats, fix_valid,
zoom, overflow
```

**Size**: ~80 bytes/row × 10 Hz ≈ 48 KB/min. A 2-minute sample ≈ 96 KB; twenty samples ≈ 2 MB against
~11.7 MB of FFat. Comfortable, but the auto-stop cap still matters.

### 8.3 The sample protocol

Ordered so that earlier samples establish the baselines that later ones are interpreted against.

| # | Label | What to do | What it determines |
|---|---|---|---|
| 1 | `flat360` | Standing still, device **flat**, slow 360° (~30 s) | `H₀`; hard-iron ripple vs heading; axis ratio; circle-fit residual |
| 2 | `phone360` | Same spot, held **phone-style**, slow 360° | How far H departs from `H₀` at the natural holding angle → the "not flat" threshold |
| 3 | `flat360` (repeat) | Same as 1, few minutes later | Repeatability — separates real drift from noise |
| 4 | `walk-straight` | Walk a straight ~100 m, phone-style, glancing occasionally | **The headline number**: compass heading vs GPS course as ground truth = the actual tilt bias while walking. Also the body-shake spectrum → the τ-per-zoom table |
| 5 | `walk-straight` | Same 100 m, deliberately held **flat** | Control for #4 — isolates tilt from every other error source |
| 6 | `stand-still` | Stand still, flat, 60 s, no rotation | Noise floor of H and of heading → the EMA/deadband floor |
| 7 | `disturbance` | Walk past a parked car / metal fence / utility box | What a real disturbance looks like, vs. a stale calibration |
| 8 | `freeform` | Tumble the device through all orientations, ~60 s | Whether a figure-8 motion can actually recover a 3-D calibration here (Level 2 feasibility), and 3-D coverage statistics |
| 9 | `shake-100hz` | Walk ~30 s with accel **and gyro logged at 100 Hz** | The body-shake spectrum at full resolution — decides whether accel-only suffices, whether a 10 Hz gyro helps, or whether 100 Hz is required (§6A.2). Deliberately short: it accepts the 100 Hz bus risk only inside a controlled recording. Capture `i2c_manager` failure counts before and after |

Samples 4 and 5 are the pair that decides whether Level 3 is worth its cost. If the bias in #4 is
small, Level 1 plus the τ work is enough and the accelerometer stays unused.

### 8.4 Analysis is offline

The CSVs get analysed on a computer, not on-device. Nothing in §5 needs to run in firmware until its
thresholds are known.

---

## 9. Related but separate

### 9.1 Zoom-dependent rotation smoothing

Distinct from calibration and independent of everything above: heavier heading smoothing at coarse
zoom (1 km), current responsiveness at 50 m. It addresses **body shake**, which is zero-mean noise
that averaging genuinely removes — not tilt, which it cannot touch.

Two points for its own proposal: zoom is **declared user intent** rather than inferred motion state
(no thresholds, no false positives, no failure at standstill — strictly better than the GPS-km/h
approach already rejected); and it is **ROADMAP FT-02, whose "won't fix" premise has expired** — that
entry reasoned from a 1 Hz compass rate, and the compass now runs at 10 Hz. Express it as **τ in
seconds per zoom level**, not α, so it survives a future rate change. Sample 4 supplies the numbers.

### 9.1a A τ regression is probably already in the code

Field report, 2026-08-01: *"since the compass is so responsive it bounces non stop."* There is an
arithmetic explanation available before any new work.

`HEADING_SMOOTHING` (`navigation.h:104`) went from **α = 0.8 at 1 Hz** to **α = 0.3 at 10 Hz**. Those
are not equivalent. Converting each to a time constant, `τ = −Δt / ln(1−α)`:

| | Δt | α | τ |
|---|---|---|---|
| Before (1 Hz) | 1.0 s | 0.8 | **0.62 s** |
| Now (10 Hz) | 0.1 s | 0.3 | **0.28 s** |

The rate increase was accompanied by an α change, but **not one that preserved τ** — the filter is now
about **2.2× less smoothing in time terms** than the behaviour it replaced. The heading did not merely
get faster to update; it got substantially twitchier, which is what the field report describes.

Preserving the original feel at 10 Hz would need `α = 1 − exp(−0.1/0.62) ≈ **0.15**`. That is a
one-constant experiment requiring no hardware work and no field data, and it is worth trying before
attributing the bounce to anything more interesting. It is also a textbook instance of the project's
recurring defect — a constant re-derived by rate but not by *meaning* — and belongs in the τ-per-zoom
table as its baseline rather than being fixed in isolation.

Caveat: α = 0.15 restores the *pre-10 Hz* feel, which is a data point, not necessarily the target. The
whole reason for going to 10 Hz was responsiveness. The τ table is what resolves that tension per zoom
level; this number just establishes where the old behaviour sat on the scale.

### 9.2 Stillness detection

If the accelerometer is ever enabled, the variance of its magnitude is a **better "am I stopped"
signal than GPS speed** — instant, works indoors, needs no fix. That is a possible answer to the
original moving-vs-stationary question with GPS not involved at all. Out of scope here; noted so it
isn't rediscovered later.

---

## 10. Open questions the data must answer

| Question | Sample | Currently |
|---|---|---|
| `H₀` and its stability | 1, 3 | **answered: ≈3000 (raw LSB), repeatable to ~1%** between samples 006/008. See [`calibration/wp3_results.md`](calibration/wp3_results.md) |
| Tilt threshold on \|H − H₀\|/H₀ | 1 vs 2 | **answered: ≈+0.23 (23% higher, not lower) at ~45–50° tilt** — the vertical field component leaks into the horizontal read as tilt grows, inflating `h_mag` rather than shrinking it |
| Stale-calibration ripple threshold | 1, 3 | **answered: 2–4% circle-fit residual, axis ratio ~1.06–1.07 on flat data is the healthy band.** Must be gated on known-flat data — tilt alone pushes residual to 11–20% and axis ratio to 1.5–1.9, which would otherwise look like a bad calibration |
| Actual tilt bias while walking | 4 vs 5 | **answered — go for Level 3.** Not a fixed bias: two samples at ~46–50° tilt gave −135° (walking N) and +4° (walking S) heading error vs GPS course, while both flat samples stayed under 8° mean / 11–16° std regardless of direction. A 2-axis compass's tilt error is heading-dependent (interacts with ~59.7° local inclination) — no lookup-table correction works, real tilt compensation is required. Full numbers: [`calibration/wp3_results.md`](calibration/wp3_results.md) |
| Is soft iron significant here? | 1 (axis ratio) | **answered: minor when flat** — axis ratio 1.06–1.17 on flat360. (Confounded by tilt on non-flat samples, see ripple threshold row) |
| Accel gravity-estimate τ | 4 | **partially answered**: shake spectrum (sample 9) peaks at ~2 Hz/~4 Hz with ~40% of energy above 5 Hz — the gravity EMA needs a τ long enough to average out 1–4 Hz walking shake; exact τ not yet fit, see wp3_results.md §"Shake spectrum" |
| τ-per-zoom smoothing table | 4, 6 | **not yet built** — inputs now available (noise floor 1.5–2.5°, flat-walk heading residual 4.81° with no shake leakage), but the table itself is still WP-7 |
| Can a figure-8 recover 3-D calibration on this hardware? | 8 | **answered: yes, feasible.** Freeform sample swept elevation −87.6°..+82.5° and full 360° azimuth in ~60 s — near-total sphere coverage |
| WMM n=1..3 intensity accuracy | — | uncharacterised; verify against a reference model |
| Mag↔**device** frame sense | — | **answered** by the vertical-inversion observation (§3.3): X rotates toward *up* |
| Mag↔**accel** frame rotation | — | **unknown; needs a dedicated bench procedure, not a field sample** |
| Is the vertical heading really confined to 180° ± 31.5°? | 2 | **structurally confirmed** at 4 azimuths (§3.3) — N and S land on 180° exactly, as predicted with no free parameters. Arc *width and sign per azimuth* still need sample 2 |
| Does accel-only suffice, or is a gyro needed — and at what rate? | 9 | **answered: accel-only suffices**, no gyro needed. But it must be oversampled (e.g. 50–100 Hz) and averaged down, not read at a flat 10 Hz — ~40% of the shake spectrum sits above a 10 Hz sample's 5 Hz Nyquist and would alias into the gravity estimate |
| Gyro power draw | — | moot — gyro not needed per the row above |
| Does α = 0.15 fix the bounce? | — | **answered: yes.** Verified on a walk 2026-08-01 — "way better". Confirms the τ regression in §9.1a was the cause |

---

## 11. Non-goals

- **Not** reinstating GPS heading fusion — ADR-0017 stands; a magnetometer works at rest and that is
  why it replaced GPS course.
- **Not** reinstating gyro sampling *as a heading source* — that is the role it structurally cannot
  fill (§6A). As a rate reference alongside the compass it is **deferred pending sample 9**, not
  ruled out.
- **Not** an automatic recalibration that runs without the user knowing. Calibration requires a
  specific motion; a silent attempt would produce a confidently wrong result, which is the exact
  failure mode this whole document exists to eliminate.
- **Not** changing the heading formula before the data exists.

---

## 12. Implementation plan

**Ordering principle: nothing whose constants are unmeasured gets built before the trip.** WP-1 exists
to produce the data; WP-4 onward consume it. Building the algorithms first would mean guessing exactly
the numbers this document exists to measure.

### WP-0 — Immediate, no field data required

Do these first; they are independent of everything else.

**0.1 — Fix the two stale declination comments.** ✅ **Done 2026-08-01.** The code is correct (`+=`, empirically verified,
`feedback_wmm_sign.md`); the comments are wrong. Do not "fix" the code.
- `include/utils/wmm_declination.h` — the `@return` block says `true_heading = magnetic_heading - declination`
- `include/settings_manager.h:69` — says `Apply: true_heading = magnetic_heading - compass_declination_deg`
- Both should read `true_heading = magnetic_heading + declination` (positive = East).

**0.2 — Heading smoothing experiment (§9.1a).** ✅ **Done and verified on hardware 2026-08-01** —
field report: *"shakiness while walking is way better with this last change."* `HEADING_SMOOTHING` is
now `0.15f` (τ ≈ 0.62 s at 10 Hz) and the comment states τ rather than only α. The render deadband
comment in `task_manager.cpp`, whose arithmetic assumed α = 0.3, was corrected at the same time (the
deadband gained margin — a single-sample ±2° excursion now attenuates to ~0.3°, not ~0.6°).
- **This changes body shake only.** Tilt error is a *bias*; no smoothing constant touches it.
- If 0.15 ever feels sluggish, the answer is the τ table (WP-7), not a hand-picked middle value.

### WP-1 — Field logging build (blocks the trip)

**Status: ✅ verified on hardware 2026-08-02.** Full START/STOP/back navigation chain exercised
repeatedly (settings → DEV → Field Log → start → stop → back → repeat, including through auto-standby)
with no crash or hang. Pre-flight testing itself caught and fixed three bugs first: a `sdkconfig.defaults`
Kconfig footgun that silently kept FATFS on 8.3 filenames (CSV writes were failing outright), and two
LVGL screen-lifecycle bugs in `goToSettingsScreen()` (two crashes + one hang, same root cause each
time — see CHANGELOG entry and `memory/lvgl_screen_lifecycle.md`). Cleared for WP-2.

**1.1 — Compass: expose the full corrected vector.** ✅ implemented
`src/hardware/sensors/compass_qmc5883l.cpp`
- Apply `cal_z_offset` in `read()` — it is currently stored and ignored (`compass_qmc5883l.cpp:113-114`).
  With the offset still 0 this changes no behaviour today; it makes the code correct for WP-5.
- Add corrected `cx, cy, cz` and `H = sqrtf(cx*cx + cy*cy)` to `CompassData`
  (`include/hardware/sensors/compass_qmc5883l.h`).
- *Acceptance*: `compass read` serial output shows raw, corrected and H; heading unchanged from before.

**1.2 — Accelerometer: new minimal driver.** ✅ implemented
New `src/hardware/sensors/accel_qmi8658.cpp` + header, modelled on `compass_qmc5883l.cpp`'s shape.
- **Do not resurrect `gyro_qmi8658.cpp` wholesale** — it enables both sensors (`CTRL7 = 0x43`), uses
  file-scope globals, and is flagged in CLAUDE.md as needing a C++ refactor. Read it for the register
  map, write a new module. It does already route through `i2c_manager` via the `I2C_Driver`
  compatibility layer (§7.2), so that pattern is safe to copy.
- Accel-only: `CTRL7 = 0x01`. Set range and ODR via `CTRL2`. Address 0x6A/0x6B — probe both.
- Read the 6-byte burst 0x35–0x3A. Add an optional 12-byte mode (0x35–0x40) that also captures the
  gyro for logging only (§6A.1) — contiguous, so it is the same transaction.
- **Runtime kill switch** — a settings flag plus a serial command, so it can be disabled in the field
  without reflashing (§7.4).
- *Acceptance*: `accel read` prints plausible values; held flat one axis reads ≈ ±1 g and the other two
  ≈ 0; `i2c_manager` failure counters unchanged over 10 minutes.

**1.3 — Wire the accel read into the System Task.** ✅ implemented
`src/utils/task_manager.cpp`, immediately after the compass read (~line 1462-1520).
- Same tick, same place. **Must obey the existing gates**: reads fully suspended while the WiFi AP is
  up, chip re-initialised after standby (`task_manager.cpp:1465-1516`). Skipping this reintroduces
  exactly the cases those gates exist for.
- Keep it inside one mutex acquisition alongside the compass where the `i2c_manager` API allows.
- **Do not move the compass read to the I2C Task** while doing this — see
  `docs/compass_i2c_constraint.md`.

**1.4 — Field log module.** ✅ implemented
New `src/utils/field_log.cpp` + header.
- API roughly: `begin()`, `startSample(label)`, `stopSample()`, `isRecording()`, `appendRow(...)`,
  `stats()`.
- **Writes must not block the System Task.** Sensor ticks push rows into a RAM ring buffer
  (16–32 KB, PSRAM); `loopTask` drains it to the file in chunks. `loopTask` already does exactly this
  class of work (GPX server, battery sampling). Putting `fwrite` on the 10 Hz sensor path would add
  filesystem latency to the sensor clock.
- Files: `/sdcard/logs/cal_<NNN>_<label>.csv`, `NNN` monotonic, discovered by scanning the directory at
  `begin()`. `/sdcard` is the FFat mount (`device_manager.cpp:545`; `LOGS_FOLDER = "/sdcard/logs"`,
  `gpx_server.cpp:29`).
- Header comment block per file: firmware build, RTC timestamp, active `compass_cal_x/y/z`,
  `compass_calibrated`, declination + validity, `H₀` if known, zoom, accel enabled, sample rate.
- Columns exactly as §8.2.
- Auto-stop on a time or size cap; refuse to start below a free-space floor.
- *Acceptance*: a 60 s sample produces a well-formed CSV of the expected size that parses cleanly in a
  spreadsheet, with no dropped rows (row count ≈ rate × duration).

**1.5 — Field Log screen (DEV mode).** ✅ implemented
`src/ui/` — follow the existing settings/overlay screen patterns.
- **START / STOP** button, large, one-handed reachable. START opens a new file, STOP closes and flushes.
- **Label selector** cycling the fixed list: `flat360`, `phone360`, `walk-straight`, `stand-still`,
  `disturbance`, `freeform`, `shake-100hz`.
- **Audible confirmation via the existing buzzer module** — chirp on start, double-chirp on stop.
  Non-negotiable: half the samples are taken while tumbling the device or holding it phone-style, i.e.
  not looking at the screen. Silent start/stop will produce empty or doubled files.
- Live readout: elapsed, row count, file size, free space.
- ⚠️ **LVGL is not thread-safe** — all of this runs on the UI Task only. Cross-task updates go through
  `task_manager::queueUIUpdate()` or `withDisplayMutex()`. See project memory; violating it hangs the
  UI Task at loop count 2.
- *Acceptance*: start/stop 5 times in a row produces exactly 5 files, correctly named, none empty.

**1.6 — High-rate mode for sample 9.** ✅ implemented
- The System Task runs at 100 ms, so it cannot produce 100 Hz. Spawn a **short-lived dedicated task**
  (Core 0, 10 ms period) that exists only while a `shake-100hz` sample is recording, and delete it on
  stop. Do not raise `SYSTEM_UPDATE_MS`.
- Log `i2c_manager` failure statistics into the file header and footer for this mode specifically.
- *Acceptance*: row count ≈ 100 × duration; no I2C failures; nothing else on the device misbehaves.

**1.7 — Make the logs downloadable.** ✅ implemented
`src/gpx/gpx_server.cpp:895` — `logs_list_handler` filters strictly on a `.log` extension, so `.csv`
files will **not appear** on the `/logs` page. Extend the filter to accept `.csv` as well (the
`/download/logs/<file>` route itself needs no change).
- *Acceptance*: the `/logs` page lists the CSVs and the browser downloads one intact.

### WP-2 — The trip

Run §8.3 samples 1–9 in order. Nothing to implement.

**Status: ✅ done 2026-08-01/02.** 18 samples captured (`cal_006`–`cal_023`, numbering continues from
the WP-1 acceptance test which used 001–005). Two rotate-left/rotate-right variants were added in the
field beyond the planned 9; sample 8 was mislabeled at capture and corrected in the filename; samples
21/22 were flagged in the field for possible discard (operator unsure of hold orientation) but were
**recovered, not discarded** — WP-3 classified them from the accelerometer instead (021 flat, 022
phone-style). Full sample-by-sample mapping, direction-of-travel notes, and these deviations are in
[`docs/calibration/README.md`](calibration/README.md).

### WP-3 — Analysis (offline, on a computer)

**Status: ✅ done 2026-08-02.** Produced `H₀` and its stability; the tilt threshold; the ripple
threshold; the axis ratio; the shake spectrum; the tilt bias from samples 4 vs 5 (plus the
accel-reclassified 21/22 as a second N/S pair); and a verdict on accel-only vs gyro from samples 9.
Results recorded into §10 above; full numbers, tables and the reproducible script are in
[`docs/calibration/wp3_results.md`](calibration/wp3_results.md) and
[`docs/calibration/analyze.py`](calibration/analyze.py). Data quality was good throughout (zero
dropped rows across all 18 files, metronomic 10 Hz/100 Hz timing) — **no retake needed.**

**Go/no-go for WP-6: go.** The tilt bias is large and heading-dependent (−135° walking N vs +4°
walking S at similar ~46–50° tilt) — not a fixed offset a lookup table could correct. Real
accelerometer-based tilt compensation is required for phone-style holding to ever be reliable.
Decision record, including why the gyro is deferred rather than folded in now:
[`docs/adr/0018-tilt-compensation-required-gyro-deferred.md`](adr/0018-tilt-compensation-required-gyro-deferred.md).

### WP-4 — Level 1: health metrics *(needs WP-3)*

**Status: ✅ done 2026-08-02.** `H₀` capture in the calibration overlay (§5.2) and circle-fit residual
+ axis ratio as quality scores (§5.3) are computed from the same sweep, exactly (via the aggregate
raw x/y sums and the *final* offset, not a running approximation) and shown live during calibration.
Runtime classification (`compass_qmc5883l::classifyHealth()`) implements the magnitude half of the
§5.1 table — `HEALTHY`/`TILTED`/`DISTURBANCE`/`UNCALIBRATED` — with a ~1s EMA and hysteresis around
the transition ratios (1.12/1.08 tilt, 0.85/0.90 disturbance) sized off the ~3% noise floor and the
~23% tilt inflation from WP-3. A HUD trust indicator surfaces it, hidden when healthy.

**Deliberately not attempted**: distinguishing a *stale calibration* from a *momentary tilt* using
live single-reading dynamics, per §5.1's caveat that the ripple threshold "must be gated on known-flat
data... not usable standalone as a tilt detector." Instead, "recalibrate?" is driven by the **stored**
calibration's own residual/axis-ratio score from save time (thresholds: residual > 5% or axis ratio
outside ~0.83–1.20 — comfortably between the healthy band and the confirmed-bad phone360 numbers), not
by anything computed live. This is an honest Level 1 output, not a gap: correcting the confusion
between stale-cal and tilt from live dynamics alone was never claimed as feasible with the data in
hand.

Not built (optional, not required for WP-5/6 to proceed): the WMM absolute cross-check (§5.4) —
`wmm_declination.cpp` still only computes horizontal X/Y, and the intensity accuracy of the n=1..3
truncation is unverified.

Build impact: +344 bytes RAM. Full writeup: CHANGELOG.md "Compass Level 1 health metrics + trust
indicator (WP-4)".

### WP-5 — Level 2: 3-axis calibration *(needs WP-4)*

New calibration motion with 3-D coverage scoring, real `cal_z_offset`, updated overlay copy and
instructions. Feasibility comes from sample 8.

### WP-6 — Level 3: tilt compensation *(needs WP-5, and a go/no-go from WP-3)*

Gravity estimate with a τ-EMA on the accel vector; the mag↔accel frame rotation determined by a bench
procedure (§10); the standard tilt-compensated heading. Signs and axis conventions fixed empirically on
hardware, not on paper.

### WP-7 — τ-per-zoom smoothing, and the gyro decision *(needs WP-3)*

τ in seconds per zoom level (§9.1), baselined on the §9.1a number. Reopen ROADMAP FT-02 with its
expired premise stated. Gyro go/no-go from sample 9 (§6A.2).

---

## 13. Notes for a fresh session

**Read in this order**: this document → `docs/compass.md` → `docs/bh880_module.md` →
`docs/compass_i2c_constraint.md` → ADR-0013, ADR-0014, ADR-0017. Then `CLAUDE.md` for build and
documentation standards.

**Start at WP-5 (Level 2: 3-axis calibration) if continuing this work.** WP-0 through WP-4 are done —
field data collected and analyzed (WP-2/WP-3), Level 1 health metrics built (WP-4, this section). WP-5
onward still need care: nothing whose constants are unmeasured gets built, though WP-5/6's inputs
(feasibility from sample 8, the tilt go/no-go from WP-3) already exist.

**Invariants that will bite:**
- **LVGL is not thread-safe.** After the tasks start, only the UI Task may call LVGL.
- **The compass read stays in the System Task** (`docs/compass_i2c_constraint.md`).
- **`I2C_PROCESS_MS = 20` is a floor with an undiagnosed cause** (ADR-0013). Do not lower it.
- **Serial requires USB**, which also powers the 5 V rail — no serial diagnostics in the field.
- **Editing `sdkconfig.defaults` does not change the build.** Delete `sdkconfig.cc-radar` and diff the
  regenerated file (`sdkconfig_regeneration.md`).
- **The four render-pipeline flags in CLAUDE.md are load-bearing.** Nothing here should touch them.

**Build**: `pio run` (env `cc-radar`, ESP-IDF), `pio run -t upload`, `pio device monitor` at 115200.

**Commit discipline**: do not commit until the user has verified on hardware. This applies especially
to WP-0.2, which is a subjective feel change.

**Documentation obligations** (CLAUDE.md standards): CHANGELOG entry per work package; build impact
measured; update this document's §10 as questions get answered; ROADMAP status summary-only with a link
here. **ADR candidates** — each is a real choice between alternatives, so each gets one when built:
- accelerometer-only vs. accel+gyro fusion for the gravity estimate (WP-6 / §6A)
- magnitude-based health detection vs. periodic forced recalibration (WP-4)
- 3-axis tumble calibration vs. staying 2-axis and cueing "hold flat" (WP-5)
- zoom-indexed τ vs. a single global smoothing constant (WP-7)

---

## References

- [`docs/compass.md`](compass.md) — current implementation
- [`docs/bh880_module.md`](bh880_module.md) — module, axes, I2C map; §"module orientation matters"
- [`docs/compass_i2c_constraint.md`](compass_i2c_constraint.md) — why the compass read lives in System Task
- [`docs/wmm_declination.md`](wmm_declination.md) — declination model
- [ADR-0013](adr/0013-i2c-process-ms-tuned-floor.md) — the undiagnosed I2C timing floor
- [ADR-0014](adr/0014-compass-stays-on-shared-i2c-bus.md) — bus decision
- [ADR-0017](adr/0017-compass-sole-heading-source.md) — compass replaces GPS heading
- [ROADMAP.md](../ROADMAP.md) — FT-02 (zoom smoothing), FT-06 (I2C freeze)
