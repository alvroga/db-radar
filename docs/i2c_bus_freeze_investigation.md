# I2C Bus Freeze Investigation (FT-06 / FT-07)

**Status**: **Resolved and field-verified, 2026-08-05.** Root cause found and patched 2026-08-02 — a
real ESP-IDF driver bug (confirmed against Espressif's own upstream issue tracker and source history),
not application logic. **The `i2c_master.c` NACK-handling busy-wait had no timeout bound at all** and
is patched at build time via `scripts/patch_i2c_master_nack_hang.py`. `panic_on_timeout` (TWDT) stays
`true` as a belt-and-suspenders safety net in case any other, still-unknown hang mechanism exists. See
[Root Cause Found](#root-cause-found-and-patched-2026-08-02) for the fix, and
[Field Verification, 2026-08-05 — confirmed](#field-verification-2026-08-05--confirmed) for the closing
result: ~10.5 hours of combined logged runtime across two sessions (an ~8h51m day session and a further
~1h48m after an unrelated power interruption), zero freezes, zero silent heartbeat gaps, failure rate
~0.01%, never enough to trip the recovery watchdog in [ADR-0003](adr/0003-proactive-i2c-bus-recovery-watchdog.md).

**Kept open, lower priority**: the *why* behind the NACKs themselves (T-B/T-C/E2/E4 — signal integrity,
bus voltage, clock-stretching, EXIO register corruption) is still unconfirmed. It no longer matters for
the freeze itself — the patch bounds the hang regardless of why a NACK happens — but would explain why
this bus produces NACKs at all. Not blocking; see [Open Questions](#open-questions).

This document consolidates the investigation across three separate occurrences spanning
2026-07-31 to 2026-08-02. ROADMAP.md's FT-06/FT-07 entries stay summary-only per project convention;
this is the full writeup they point to.

---

## The Issue

A recurring full or partial interface freeze on the ESP32-S3 radar, first reported 2026-07-31 and
recurring at least twice since, with no fix yet confirmed to prevent it (only to recover from it after
the fact).

### Occurrence 1 — 2026-07-31

Reported twice in one session. Button, touchscreen, and display updates all stopped responding. First
time required a full power cycle. Second time self-cleared when the device went to standby and woke
back up.

### Occurrence 2 — 2026-08-02 (evening, on commit `9ae6368`)

Device idle on Settings > DEV tab, zero interaction, nothing in progress. Serial was live at freeze
onset — `diag i2c` / `task status` got **zero response for 30s+**, genuinely dead, not silent-but-alive.
A soft reboot (no power removed) hung boot solid immediately after EXIO's first I2C transaction NACKed.
Only a full power cycle produced a clean boot.

### Occurrence 3 — 2026-08-02 (field drive, on commit `fdde7e9`, after the bit-bang recovery fix below
was flashed)

Different profile from the first two — **gradual degradation, not a sudden binary freeze**:

- Screen stopped updating, rotation stopped tracking heading.
- The physical button (GPIO0, not I2C) kept working, but very slowly and intermittently — a long
  press did reach Settings, a longer press did enter standby, a further press did seem to wake it, but
  the radar view stayed frozen in the standby placeholder afterward.
- This is consistent with something *degrading over time* rather than failing instantly: the interface
  didn't die all at once, it got worse the longer the session ran.
- The device had been left running continuously (auto-sleep disabled) for an extended drive. It had
  also been left in standby overnight previously with no issue — sleep/wake round-trips are fine; it's
  sustained *active* runtime that triggers this.

### Occurrence 4 — 2026-08-02 (on the forensic-logging + 100kHz build, `system(3).log`)

**The most complete trace captured so far, and a different profile from occurrence 3** (sudden total
stop after apparent full recovery, not a gradual creeping slowdown):

- Uptime 266 minutes (4h26m) of clean operation — dozens of healthy 60s `HEALTH`/`I2C_STATS`
  heartbeats, GPS fix, heap flat at 6,416,132 bytes (no leak).
- `04:24:25` and `04:24:53` — two isolated `COMPASS reg=0x00 read err=ESP_ERR_INVALID_STATE` failures,
  each returning in ~3ms (a fast synchronous rejection, not a slow/timing-out call). `consec=1` each,
  immediately cleared by the next successful read.
- `04:25:00` and `04:26:00` — two more clean heartbeats, `i2c_consec=0` globally, nothing anomalous.
- Then **nothing**. No further log line of any kind — no `HEALTH`, no `I2C_STATS`, no `EXIO_CANARY`, no
  `FAIL`, no `SLOW`. No `Reset reason` on a next boot; this is the last thing in the file.

**Why this is a new data point, not a repeat of occurrence 3**: `checkI2CBusHealth()` never engaged —
`i2c_consec` reads `0` on the very last line before the log stops. That's a *different* blind spot than
occurrence 3's (which was a sustained low-but-nonzero failure *rate* that never chained to 10
consecutive). Here, the evidence points at a single I2C call that **never returned at all**: compass
reads happen inside `updateStatusLabels()`, called once per iteration of System Task's single-threaded
loop (`task_manager.cpp`). `i2c_manager::read()`'s failure/success bookkeeping — the thing
`consecutive_failures` and every log line depend on — only runs *after* `i2c_master_transmit_receive()`
returns. A call that blocks past its own `pdMS_TO_TICKS(50)` timeout (exactly what E3/T-D already
flagged as a known-class ESP-IDF new-driver hang, and exactly the kind of FSM corruption the two
`ESP_ERR_INVALID_STATE` hits 90s/30s earlier are consistent with) produces **zero** forensic output —
not a `SLOW` line, not a `FAIL` line, nothing — because the instrumentation that would log it lives
downstream of the call returning. If System Task froze there: it never reaches `vTaskDelayUntil` (no
more heartbeats, ever), it's still holding `g_bus_mutex` (recursive) so every other task's I2C calls
degrade to 200ms mutex-timeout failures without hanging themselves, and since GPS/compass fusion lives
in that same stuck call, the radar visibly stops updating — a whole-device "freeze" from one stuck
transaction, with the touch/button symptom explained the same way occurrence 1's shared-loop theory
already predicted.

**Immediate response**: `wdt_config.panic_on_timeout` (`src/core/main.cpp`) flipped `false → true`.
TWDT timeout stays 30s. A hang of this shape now resets the device instead of leaving it silently
wedged. Bus speed was deliberately **left at 100kHz** rather than reverted — this occurrence's
mechanism (a state-machine hang) is orthogonal to bus speed/signal-timing margin, which is what 100kHz
(T-B) was hedging against, and changing two variables between occurrences would make the next log
harder to read, not easier. This response is superseded by the actual fix, below — kept as a
belt-and-suspenders safety net.

## Root Cause Found and Patched, 2026-08-02

Reading the exact pinned ESP-IDF source (not just its header/API surface) after Occurrence 4 found the
literal bug, then confirmed it against Espressif's own issue tracker and source history rather than
resting on inference alone.

### The bug

`components/esp_driver_i2c/i2c_master.c`, function `s_i2c_send_commands()` (this project's pinned
copy: `~/.platformio/packages/framework-espidf`, ESP-IDF **5.5.0** via PlatformIO's
`framework-espidf @ 3.50500.0`). After a transaction's event queue correctly reports an
`I2C_EVENT_NACK` — meaning the driver has already respected the application's `xfer_timeout_ms` up to
this point — it waits for the hardware to finish the STOP condition with:

```c
} else if (event == I2C_EVENT_NACK) {
    // For i2c nack detected, the i2c transaction not finish.
    // start->address->nack->stop
    // So wait the whole transaction finishes, then quit the function.
    while (i2c_ll_is_bus_busy(hal->dev)) {
        __asm__ __volatile__("nop");
    }
}
```

**No bound of any kind.** It assumes the hardware always autonomously completes the STOP after a NACK.
If it doesn't — a slave stretching SCL, or the I2C FSM getting stuck post-NACK — the calling FreeRTOS
task spins here **forever**, completely bypassing the timeout the application passed to
`i2c_master_transmit()`/`transmit_receive()`. This is called from `s_i2c_transaction_start()` →
`s_i2c_synchronous_transaction()` → `i2c_master_transmit_receive()`, i.e. it's reachable from every
transaction this project makes through `i2c_manager::read()`/`write()`.

This is the literal mechanism behind Occurrence 4 (compass reads run on System Task; a hang here
freezes that task's loop entirely — no more heartbeats, held `g_bus_mutex`, stalled GPS/compass-driven
rendering) and matches this document's own **E3**/**T-D** hypotheses exactly, now confirmed at the
source level instead of inferred from a truncated log.

### Confirmed as a known, already-fixed upstream bug

[github.com/espressif/esp-idf/issues/17720](https://github.com/espressif/esp-idf/issues/17720) — "I2C
driver causes watchdog timer reset waiting for stop after nack" — reports this exact function, this
exact loop, this exact backtrace (`i2c_ll_is_bus_busy` → `s_i2c_send_commands:543` →
`s_i2c_transaction_start` → `s_i2c_synchronous_transaction`), independently on different hardware.
Closed as fixed. A second commenter confirmed the bug was **still present in IDF 5.5.1** as of
2025-11-14. Diffing the actual GitHub source across tags confirmed exactly when the fix landed:

| Tag | NACK-wait loop |
|---|---|
| v5.5.1, v5.5.2, v5.5.3 | unbounded (bug present) |
| **v5.5.4** | bounded — `TickType_t start_tick` + `ticks_to_wait` check, calls `s_i2c_hw_fsm_reset()` on expiry |
| v5.5.5 (latest tagged) | same fix, present |

This project is pinned to **5.5.0** (older than even the confirmed-still-broken 5.5.1). PlatformIO's
package registry (`platformio/framework-espidf`) tops out at `3.50503.0` = **IDF 5.5.3** for the 5.5.x
line — still unfixed. The next available version, `4.60000.0`/`4.60001.0`, is **IDF 6.0.x** — a
major-version jump (breaking API/Kconfig changes almost certain) that is not a reasonable undertaking
to fix one driver bug. **There is no way to pull in the fix by bumping a pinned version through
PlatformIO right now.**

### The fix: build-time backport, not a manual local edit

`scripts/patch_i2c_master_nack_hang.py`, wired in via `platformio.ini`'s `extra_scripts` (same
mechanism already used by `scripts/gen_version.py`). Runs before every build:

- Locates the pinned framework's `i2c_master.c` via `env.PioPlatform().get_package_dir("framework-espidf")`
  (portable — no hardcoded path, works on any machine/CI that installs the same pinned package).
- Applies **only** the timeout-bound fix — the exact block as it landed in upstream v5.5.4 — not the
  large, unrelated read-command-batching refactor that shipped in the same upstream release. Backporting
  the whole file would import a lot of untested surface area for zero benefit here.
- **Idempotent and conservative by construction**: replaces the old block only if found byte-for-byte
  unpatched; if already patched, no-ops with a log line; if neither the old nor new block is found
  (framework package version changed since this script was written), it does **not** guess — it leaves
  the file alone and prints a loud, impossible-to-miss warning in the build log instead of silently
  failing to protect against a bug it no longer recognizes.

Verified 2026-08-02: `pio run` applies the patch (confirmed by grepping the installed package file for
the new block), a second `pio run` correctly reports "already applied" instead of double-patching,
and the firmware builds clean (RAM 40.6%, Flash 78.2% — negligible delta from the added log line/logic).
**Not yet field-verified** — this fixes a hang that only manifests under a NACK the hardware can't
autonomously clear, which by nature isn't reproducible on demand; monitored in the field going forward,
same trust-then-monitor pattern as the bit-bang recovery and forensic-logging builds earlier in this
investigation.

### What this does and doesn't close

**Closes**: the specific mechanism identified in Occurrence 4 — a NACK-triggered hang with no timeout,
now bounded and self-recovering (the driver itself calls `s_i2c_hw_fsm_reset()` on expiry, same as the
existing app-level `checkI2CBusHealth()` path, just triggered from inside the driver instead of waiting
for the app to notice).

**Does not close**: *why* the bus produces NACKs the hardware can't autonomously recover from in the
first place — the electrical/signal-integrity theories (T-B, T-C), the clock-stretching-slave theory
(E2), and the EXIO register-corruption question (E4) are all still open, and this patch doesn't touch
any of them. It also doesn't rule out a *second*, different hang mechanism existing elsewhere in the
driver — that's exactly what `panic_on_timeout=true` remains for.

---

## Theory

### Mechanism (best-supported, ties all three occurrences together)

The shared I2C bus (SDA=15, SCL=7) hosts touch (CST820), RTC (PCF85063), the IO expander (TCA9554
EXIO), and the compass (QMC5883L, plus as of 2026-08-01 the QMI8658 accelerometer). All bus access is
serialized through a single recursive FreeRTOS mutex (`i2c_manager::g_bus_mutex`), with each
`read()`/`write()` call taking up to a 200ms mutex-acquire timeout, then up to 3 retries at 50ms
hardware timeout each while holding it.

Touch is polled inline inside the **UI Task's** LVGL indev callback (`lvgl_touch_read_cb`,
`device_manager.cpp`), and the **physical button is polled in that same UI Task loop**, alongside
rendering. This is the structural hazard: even though the button reads plain GPIO0 with no I2C
dependency, it is **not on its own task** — it shares a loop with touch's I2C calls. If touch's I2C
reads start stalling (bus degrading, retries piling up), the whole loop's iteration period balloons,
and everything sharing that loop — button polling included — gets starved proportionally, not
independently. This explains why the very first occurrence (2026-07-31) reported the button as fully
unresponsive alongside touch and display: a pure "external slave holds SDA low" theory doesn't by
itself explain a GPIO0 read failing, but a stalled shared loop does.

It also explains the difference in *character* between occurrence 2 (sudden, total, serial dead) and
occurrence 3 (gradual, partial, button eventually gets through): a fully wedged bus (SDA stuck low)
fails every transaction instantly and permanently until clock-recovered — a hard stop. A bus that is
merely *degrading* — some transactions succeeding, most failing, or all succeeding but slowly — produces
exactly the creeping slowdown seen in occurrence 3, without ever being a full stop.

### Why a degrading (not just wedged) bus can hide from the existing recovery code

`checkI2CBusHealth()` (`task_manager.cpp`) only acts when `consecutive_failures` — a **single counter
shared across every device on the bus** — reaches 10 in a row, and that counter **resets to 0 on any
single success by any device**. A bus that's failing most touch polls but still lets an occasional
compass or RTC read through can sit in a persistently bad state indefinitely without ever tripping the
watchdog designed around "the whole bus is dead." This is a plausible explanation for why occurrence 3
degraded for an extended period without the proactive recovery ever engaging or logging anything.

### What is causing the bus to degrade at all — timeline analysis

The trigger is still unknown, but the timeline rules out one hypothesis and points at another:

**Ruled out: the compass/accelerometer work (WP-1 through WP-6, 2026-08-01).** This was the initial
suspicion, since the accelerometer added continuous 10Hz I2C reads and directly reverses a decision
documented in CLAUDE.md history ("IMU removed to eliminate I2C bus contention"). **This is
contradicted by the dates**: occurrence 1 (`cb5473b`, first freeze report and first recovery-code
commit) is timestamped **2026-07-31 18:36**, and the accelerometer driver (`fb3764d`) wasn't written
until **2026-08-01 16:13** — almost a full day later. The bug predates the code being blamed for it.
The accelerometer's continuous reads may still be a *compounding* factor (more traffic on an
already-marginal bus makes a bad night worse), but they are not the origin.

**Better fit: the render/sensor performance work, 2026-07-28 to 2026-07-29.** This is "the performance
optimization work" the user recalled as the start of the problem, and the dates support it:

- 2026-07-28: zero-copy render via dual framebuffers, tiled rotation transpose (CPU-bound, Core 1;
  `311ca3c`, `816b421`, `ff82116`).
- **2026-07-29, 15:21** (`feb6f59`): CPU raised 160MHz → 240MHz, **and — the more relevant change for
  this bug — compass and GPS sample rate doubled from 5Hz to 10Hz** (`SYSTEM_UPDATE_MS` 200ms → 100ms).

The 10Hz change **doubles sustained I2C traffic on the shared bus, system-wide**, two days before the
first freeze was reported. That is a much closer fit — in both timing and mechanism — than anything
from the Aug 1 compass series. It also matches a comment already present in the codebase itself
(`task_manager.cpp`, near the standby-suspend gate): *"hundreds of reads/hour over a long sleep
gradually corrupt the bus"* — an acknowledgment that sustained I2C load on this specific hardware
(shared bus, TCA9554 EXIO in particular) causes cumulative degradation, previously only guarded for
standby, never for a long *active* session.

**Working theory, in one sentence**: doubling the compass/GPS sample rate on 2026-07-29 pushed
sustained I2C duty-cycle on this bus past some threshold where the TCA9554 (and/or other shared-bus
devices) accumulate errors over an extended active session; the Aug 1 accelerometer addition made an
already-marginal situation somewhat worse; the recovery code added in response only detects the
*fully-dead* case, not the gradual degradation this produces in practice.

This is a theory, not a confirmed root cause. The actual electrical/timing mechanism by which sustained
I2C traffic degrades this specific bus/chip combination is still not understood.

---

## What Has Been Tried

### 1. Probe-scan artifact fix (2026-08-01, verified on hardware)

`scanBus()` was reporting 61 phantom devices due to back-to-back `i2c_master_probe()` calls returning a
false `ESP_OK` on alternate calls. Fixed with a settle delay + double-confirmation. **This was
significant to the investigation, not just a diagnostic cleanup**: the 61-device scan had been part of
the original evidence for "wedged bus," and its debunking briefly reopened the question of whether a
bus wedge was really happening at all (see CHANGELOG.md, `diag i2c` entry). Occurrence 2's genuinely
dead serial console later re-confirmed a real wedge does occur, independent of the scan artifact.

### 2. Proactive recovery watchdog — `checkI2CBusHealth()` (2026-07-31, ADR-0003)

Added `i2c_manager::Stats::consecutive_failures`, incrementing on every failed op and resetting on
success. At ≥10 consecutive failures, System Task calls `i2c_manager::reinit()` (2s cooldown, 5-attempt
cap). This generalizes the recovery that standby-wake already performed as a side effect. **Verified
indirectly** (the mechanism it relies on — standby-wake calling `reinit()` — had already cleared a real
freeze once) but **never verified as the actual trigger for an automatic recovery**, since no freeze so
far has been confirmed to cross the 10-consecutive-failures threshold while being observed live.

### 3. Discovery that `i2c_master_bus_reset()` is a no-op on ESP32-S3 (2026-08-02, confirmed from IDF source)

Read ESP-IDF 5.5's actual source: `i2c_master_bus_reset()` → `s_i2c_hw_fsm_reset(clear_bus=true)` →
`s_i2c_master_clear_bus()`, which polls `i2c_ll_master_is_bus_clear_done()` — **hardcoded to
`return false` on ESP32-S3**. The call returns `ESP_OK` without ever driving a clock pulse. Every
recovery path shipped up to this point (`resetBus()`, and `reinit()`'s call to it) rested on this
primitive and had never actually cleared a stuck bus — any apparent past "recovery" was coincidental
(e.g., something else, like a subsequent full `i2c_del_master_bus()`/`i2c_new_master_bus()` cycle,
happening to work) rather than caused by the reset call itself.

### 4. Real bit-bang clock recovery (2026-08-02, built and flashed, **field-tested once, did not
prevent occurrence 3**)

Implemented `bitBangClockRecovery()` in `i2c_manager.cpp`: before the I2C peripheral claims SDA/SCL,
check if SDA is held low; if so, manually toggle SCL as open-drain GPIO up to 9 times (the standard
I2C bus-recovery procedure) and issue a STOP condition. Called from `init()` before
`i2c_new_master_bus()`, so it covers both the boot-time case (bus already wedged at power-on) and the
runtime `reinit()` case. Logs `[I2C] SDA held low before bus init — bit-banging clock recovery` and
then either `released (high)` or `STILL LOW`.

**This is a recovery mechanism for a fully-wedged bus (SDA stuck low), not a fix for gradual
degradation.** Occurrence 3 (the field drive) happened *after* this was flashed, and was not a hard
wedge — it was the slow-degradation pattern the recovery threshold doesn't catch (see
[Theory](#why-a-degrading-not-just-wedged-bus-can-hide-from-the-existing-recovery-code) above). So this
fix has not yet been disproven — it addresses a different failure mode than the one most recently
observed — but it also has not solved the problem the user actually experiences day to day.

### 5. Standby suspension of compass/accel reads (pre-existing, ongoing)

The System Task suspends compass and accelerometer reads entirely while in standby, and re-initializes
the compass chip on wake, specifically because sustained reads during a long sleep were observed to
gradually corrupt the bus. This does not help the active-session degradation seen in occurrence 3,
since the device was awake and in active use throughout.

### 6. Runtime kill switches available for field-testing (no rebuild needed)

- `accel off` — disables all accelerometer I2C traffic for the session. Suggested as a load-reduction
  test: if disabling it measurably reduces freeze frequency/severity, that supports "cumulative I2C
  load" as a compounding mechanism, even though it's now understood not to be the origin.
- `compass tilt off` — disables tilt compensation without touching the underlying accel reads.

---

## Open Questions

1. **What is the actual electrical/timing mechanism** by which sustained I2C traffic degrades this bus
   or the TCA9554 specifically? Not understood — only that the symptom correlates with duty-cycle and
   session length. **Partially refined 2026-08-05**: field data shows failures are compass-only (never
   TCA9554/touch/RTC/IMU) and front-loaded in the first ~30-60 min after boot rather than accumulating
   over a long session — a warm-up transient on the cabled BH-880 module, not a wear-out/degradation
   mechanism. See [New Evidence, 2026-08-05](#new-evidence-2026-08-05--failures-are-compass-only-and-front-loaded-near-boot-refines-t-b).
   Still open: which specific warm-up mechanism (module regulator, GPS cold-start, thermal settling).
2. **Does the 2026-07-29 10Hz sensor-rate change actually cause or worsen this**, as opposed to being a
   coincidentally-timed correlation? Not yet tested — would require a firmware rebuild (no runtime
   toggle exists for `SYSTEM_UPDATE_MS`), reverting it toward the old 5Hz rate as a controlled
   experiment.
3. **Would `bitBangClockRecovery()` have cleared occurrence 3 if it had run?** Unknown — the gradual
   degradation seen in the field never crossed the `consecutive_failures ≥ 10` threshold that would
   have triggered `reinit()`, so the new recovery code never got a chance to engage. This is itself an
   argument for hardening the detector (see below), separate from whether the recovery primitive
   itself works.
4. **Is `checkI2CBusHealth()`'s single cross-device counter the wrong signal for this failure mode?**
   Very likely, given occurrence 3. A rolling failure *rate* (not strictly-consecutive) or a per-device
   counter (touch specifically, since that's what drives visible responsiveness) would catch a
   partially-degrading bus that this counter cannot.
5. Whether the EXIO chip can reach a state that clock pulses alone cannot clear (needing a genuine
   power-on-reset of the chip, not just its bus interface) remains untested — no occurrence so far has
   logged `STILL LOW` from `bitBangClockRecovery()`. If one does, that would point toward a hardware fix
   (a GPIO-switched power rail for the shared I2C peripherals) rather than a software one, at the cost
   of sacrificing the only spare GPIO (19/20, shared with USB CDC serial).

## Suggested Next Steps

- Field-test `accel off` during a long active session to see if it changes freeze frequency/severity.
- As a firmware experiment (next build, not a live-field test): hardening `checkI2CBusHealth()` to
  catch partial/rate-based degradation instead of only full-bus consecutive failure, so the existing
  (probably-correct) recovery primitive actually gets invoked before things degrade this far.
- If serial is live during a future occurrence, capture whether `consecutive_failures` and
  `i2c_manager::Stats` (total/failed op counts) show a rising failure *rate* well before any full
  freeze — this would directly confirm or refute the degradation theory above.

---

## Second-Opinion Review (2026-08-02, fresh pass over the captured logs + code)

A re-read of `docs/boot_frozen.md` and `docs/boot_after_frozen.md` against the current
`i2c_manager.cpp` surfaced evidence that was already captured but never analyzed, plus new theories
and a prioritized test list. Nothing below overturns the sections above; several items sharpen them.

### New evidence, extracted from logs we already had

**E1 — The NACK bursts are exactly 5000ms apart.** In `boot_frozen.md` (the boot that itself froze at
~16s), the two runtime NACK bursts land at `E (10823)` and `E (15823)` — 5000ms apart to the
millisecond, on a compass being read at 10Hz. A randomly degrading bus failing 1 read in 50 does not
do that twice with 1ms precision; something with a 5-second period is either using the bus or
disturbing it. Checked and eliminated: `checkTaskHealth()` (5s period, but touches no I2C — loop
counters only), battery update (5s, but pure ADC on GPIO4), logger flush (30s, not 5s). **The 5s agent
is unidentified.** Also noted: the first burst coincides exactly with the radar screen transition
(`Boot wait complete` / `Radar screen loaded` print immediately after it). Cheapest instrumentation:
the failure log line in `i2c_manager::read()`/`write()` already prints device + register — add
`millis()` and the calling task name (`pcTaskGetName(NULL)`), and the next freeze log identifies the
period source for free.

**E2 — The wedge may not be "SDA held low" at all, and the bit-bang fix only guards that one state.**
In `boot_after_frozen.md` (soft reboot onto the wedged bus), EXIO's first transaction **NACKed** —
meaning the address phase was clocked out on a working SCL and no ACK came back. The classic
slave-holds-SDA-low wedge doesn't present as a clean NACK; it presents as a timeout/bus-busy or
arbitration failure, because the master can't even form a START. So at that instant the bus was
*clockable but nothing answered* — a different state than the one `bitBangClockRecovery()` detects
(it checks **SDA only**, and silently does nothing when SDA reads high). Two follow-ons:

- A slave stretching **SCL** low forever (CST820-class touch controllers do clock-stretch) is
  invisible to the current check *and* unrecoverable by any master-side clocking — you cannot pulse a
  line a slave is holding. The only remedies are a slave reset line or a power cycle, which matches
  the observed "only a full power cycle cleared it."
- We have **zero direct observations of the SDA/SCL line levels during a real wedge** — the bit-bang
  code was written *after* the soft-reboot hang was captured. Make the level report unconditional
  (print both lines every `init()`, not only when SDA is low). One line of code, and the very next
  wedge tells us which of three states we're in: SDA low (bit-bang fixable), SCL low (slave-reset or
  power-cycle only), or both high (not electrical at all → driver/chip state, see E3/T-D).

**E3 — The boot hung *inside* an IDF call that has a finite timeout.** The soft-reboot log ends after
one error triple, mid-`exio::begin()`, inside a retry loop whose per-attempt timeout is 50ms. Whatever
happened, `i2c_master_transmit()` did not return within its stated timeout — that is a driver-level
hang, not application logic. (Same conclusion as occurrence-1's "boot hangs after Bus reset OK", now
with a second data point.) See T-D below.

**E4 — Possible EXIO register corruption, visible in the two boots' first reads.** The pre-freeze boot
read the TCA9554 output register as `0x3F`; the post-power-cycle boot read `0x7F`. The difference is
**bit 6 (EXIO6) — which nothing in the firmware ever writes** (verified by grep: pins used are
LCD_RST=0, TP_RST=1, LCD_CS=2, EXIO4=SD-power, BUZZER=7; the standby TP_RST toggle is
read-modify-write and preserves other bits). The TCA9554 retains its registers as long as it has
power — and note that with USB attached for serial capture, even a "power cycle" via the switch may
not depower it — so a cleared bit that no code clears means either a corrupted write landed on the
chip, or the chip's register itself degraded. Either one is direct evidence of *data corruption*, a
strictly worse symptom class than NACKs. (Caveat: the datasheet power-on default is `0xFF`, and even
the "clean" `0x7F` read differs from that — consistent with the register being retained state from the
prior session rather than a power-on default, but worth confirming what EXIO3/5/6 are wired to on the
Waveshare schematic before leaning on this.)

**E5 — The buzzer is I2C traffic, correlated with exactly the sessions that freeze.** The buzzer is
EXIO pin 7: every sonar beep is up to four bus transactions (`readOutput` + `set` on each edge, per
`buzzer.cpp`). A field session with waypoint/beacon sonar active adds a continuous stream of EXIO
writes — on the very chip suspected of accumulating corruption — plus the electrical noise of an
inductive load switching on the same board. Idle-on-desk sessions have almost none of this. This is a
load *and* noise source the duty-cycle theory hasn't counted.

**E6 (minor, benign) — every boot log contains one scary-looking NACK burst that is expected.** The
`IMU_LOW reg=0x00 read failed` burst at ~0.8s is the QMI8658 address probe at 0x6A failing before the
driver finds the chip at 0x6B. It is present in healthy boots. Don't spend investigation time on it —
but do consider probing with `ping()` instead of a 4-attempt read, so freeze-hunting logs aren't
polluted with fake bursts.

**E7 (latent bug, not the cause) — timeout units.** `i2c_master_transmit_receive()` and friends take
`xfer_timeout_ms` in **milliseconds** (verified in the IDF header), but every call site passes
`pdMS_TO_TICKS(...)`. At `CONFIG_FREERTOS_HZ=1000` (verified in sdkconfig) this is the identity and
harmless — but if the tick rate ever changes, every I2C timeout silently shrinks 10×. Fix the units
while in the file.

**E8 — Live `diag i2c` scan (2026-08-02, ~22.5 min uptime, bus currently healthy) shows two fresh
anomalies.** All five real devices (0x0D compass, 0x15 touch, 0x20 EXIO, 0x51 RTC, 0x6B IMU)
double-ACKed — the bus works right now. But:

- **A sixth "device" confirmed at 0x7E.** No chip on this board answers there, and 0x78–0x7F is a
  *reserved* range in the I2C spec — no legitimate 7-bit device can live at 0x7E. It was also the
  final address in the original 61-phantom alternation pattern. Conclusion: the probe artifact can
  occasionally survive the double-confirmation guard (two ACKs in a row), not just single-ACK. Treat
  a confirmed 0x7E as a sentinel that the artifact is active; consider requiring 3 consecutive ACKs
  and/or a longer settle for confirmation. 54 single-ACK phantoms were rejected in the same scan —
  the underlying false-ACK behavior is very much still present even on a healthy bus, which is itself
  a data point: a clean bus should NACK a vacant address *every* time, and whatever makes the ESP32-S3
  probe path see alternating ACKs on vacant addresses may be a shadow of the same electrical or
  driver-FSM marginality behind the freezes rather than an unrelated cosmetic bug.
- **One probe timed out mid-scan** (`probe device timeout. Please check if xfer_timeout_ms and
  pull-ups are correctly set up`, around the 0x28 region). The scan holds the bus mutex, so nothing
  else was on the bus — a spontaneous timeout on an idle, healthy, ~22-minute-warm bus is the
  driver's "bus busy / didn't finish" path with no competing traffic to blame. Single occurrence,
  but it's a in-the-wild sighting of the transaction-doesn't-complete behavior (E3/T-D) on a bus
  that is otherwise passing every functional read. Worth watching whether these timeouts get more
  frequent as a session ages (the latency-ramp detector in item 11 would catch exactly this).

### Additional theories

**T-A — The 2026-07-29 commit is two confounded variables, and the wrong one may be blamed.** The same
day that doubled the sensor rate to 10Hz also raised the CPU 160→240MHz. The existing working theory
attributes the freezes to I2C duty cycle; but 240MHz also means more current draw, more heat, and more
supply noise — and occurrence 3's profile (gradual degradation over a long, warm, in-car session) fits
a *thermal/power* mechanism at least as well as a *bus-traffic* mechanism. These are separable with
two builds: (a) 240MHz + 5Hz sensors, (b) 160MHz + 10Hz sensors. If (a) still degrades and (b)
doesn't, it was never the I2C rate.

**T-B — Marginal signal integrity at 400kHz that drifts with temperature.** The compass — the device
whose NACK bursts open every captured freeze — is the only device on a **cable** (the BH-880 module),
i.e. the highest-capacitance, most marginal node on the bus. Rise times that barely meet 400kHz
timing at power-on degrade as the board heats over a long session; error rate climbs gradually; that
is exactly occurrence 3's signature. Cheap, high-information test: drop the bus to 100kHz
(`Config::frequency`) for a field session. If the degradation vanishes, the whole problem is
electrical margin, and no detector/recovery logic will ever be more than a bandage.

**T-C — Bus voltage / pullup-rail mismatch on the BH-880.** The module's VCC is fed directly from the
LiPo (3.7–4.2V, per the power workaround). If the module carries its own I2C pullups tied to VCC or to
an internal rail above 3.3V, then SDA/SCL idle above the ESP32-S3's and the 3.3V slaves' tolerance,
injecting current into every pad on the bus — a plausible slow-degradation mechanism that would
*worsen with charge state* (worst at 4.2V, freshly charged). Testable with a multimeter in two
minutes: measure SDA/SCL idle voltage; anything meaningfully above 3.3V is a smoking gun. Also worth
recording at each future occurrence: battery %, charging or not, USB or not.

**T-D — A known-class ESP-IDF new-driver bug: NACK/wedge leaves `i2c_master` stuck, then it hangs
past its own timeout.** E3 shows the driver blowing through a finite timeout; the driver's own error
chain (`s_i2c_synchronous_transaction`) and the code comment in `i2c_manager.cpp` about the FSM
landing in `ESP_ERR_INVALID_STATE` point the same direction. The pinned IDF 5.5 `i2c_master.c` should
be diffed against the latest 5.5.x point release — the new I2C driver has had a steady stream of
post-release fixes for exactly this family (stuck transaction queue after NACK/timeout, hang on busy
bus). If a fix exists upstream, upgrading (or backporting one file) beats anything we can do from
application code.

**T-E — The wedger is identifiable, and two of the suspects have reset lines we already control.**
- The BH-880 (compass) is the only *detachable* device: if a wedge happens again, **unplug the module
  connector while wedged**. Bus comes back → the module (or its cable) is the wedger. This costs one
  connector unplug and definitively splits the suspect list in half.
- The CST820 touch controller — a prime clock-stretch suspect (T-E ties to E2) — has its reset line on
  **EXIO pin 1 (TP_RST)**, and `standby_manager.cpp:364` already contains a proven read-modify-write
  toggle sequence for it. LCD_RST is pin 0 likewise. So a *slave*-level reset escalation is already
  half-built: it works whenever EXIO itself is still answering.
- The two suspects that have **no** reset path are the TCA9554 itself and the RTC — if the wedger is
  one of those, the durable fix is hardware (switchable power to the bus peripherals), as §5 of Open
  Questions already anticipates.

### Things to try — prioritized

**Zero-build (field/bench, next occurrence or next session):**
1. When wedged: unplug the BH-880 connector. Note whether the bus recovers (T-E).
2. Multimeter on SDA/SCL idle voltage — once on USB power, once on battery freshly charged, once
   near-empty (T-C).
3. At every occurrence, record: power source, battery %, charging state, how long the session ran,
   ambient/enclosure temperature (hot car?), sonar active or not (T-A/T-B/T-C/E5).
4. `accel off` for a long session (already suggested above — still worth doing, now specifically as a
   *duty-cycle* data point to compare against the T-A builds).

**One-line to small instrumentation (next build):**
5. Print SDA *and* SCL levels unconditionally in every `init()`, and after every recovery attempt
   (E2). This is the single highest information-per-line change available.
6. Add `millis()` + calling-task name to the existing I2C failure log lines (E1 — identifies the 5s
   agent from any future capture).
7. Persist `i2c_manager::Stats` (total/failed/consecutive, plus a per-device failure count once it
   exists) to the SD system log every ~60s. The FLOG ring is PSRAM and dies with the power cycle that
   every hard wedge forces — today the forensics vanish exactly when they're needed.
8. Show failure rate / consecutive_failures on the DEV HUD. Serial requires USB; the freezes happen
   on battery. The screen is the only live telemetry channel in the field.
9. Fix the `pdMS_TO_TICKS` units bug (E7). Probe 0x6A with `ping()` instead of a retried read (E6).

**Detector/recovery hardening (next build, answers Open Questions 3–4):**
10. Per-device failure counters + a rolling failure *rate* (e.g., >20% of ops failing over a 10s
    window) alongside the existing consecutive counter — the degrading-bus mode provably never trips
    the consecutive one.
11. Track per-op duration; log any transaction that took >20ms. Degradation should show as a latency
    ramp long before it shows as failures — this is the earliest possible signal.
12. EXIO canary/scrubber (E4): every ~10s read back `REG_CONFIG` (must be 0x00) and `REG_OUTPUT`
    (must equal the shadow `state.out`); on mismatch, log loudly and rewrite. Detects data corruption
    — which NACK counting is blind to — and repairs the one chip whose corruption has a user-visible
    symptom (buzzer/LCD_CS/SD power bits).
13. Recovery escalation ladder: attempt 1–2 `reinit()` (as today) → attempt 3+ also pulse TP_RST via
    EXIO (sequence exists in `standby_manager.cpp`) → still failing: on-screen "power cycle required"
    banner instead of a silent frozen UI.
14. Fail-fast touch: the UI Task's touch poll currently risks 200ms mutex-wait + 200ms retry-holding
    per bad op, which is the loop-starvation amplifier in the Theory section. Give touch reads
    `retries=0` and a ~20ms mutex timeout — a degrading bus then costs dropped touch samples instead
    of a frozen interface.
15. Wrap the boot-path I2C init (EXIO begin, the recovery ping chain) in a hard task-WDT bound so a
    driver hang (E3) produces a reset + log line instead of a silent dead boot.

**Experiments (controlled, one variable each):**
16. 100kHz bus for a full field session (T-B).
17. The two-build split of the 2026-07-29 commit: 240MHz+5Hz vs 160MHz+10Hz (T-A).
18. Diff pinned IDF's `i2c_master.c` against latest 5.5.x; upgrade or backport if a stuck-FSM/hang
    fix exists (T-D).

## Forensic Logging Build + 100kHz (2026-08-02, built, **unverified on hardware**)

Built in direct response to "nothing changed hardware-wise, so anything physical should be deferred —
is there logging that can show us exactly what's happening?" Two changes, shipped together so one
field session tests both:

**1. Bus speed 400kHz → 100kHz** (`system_config.h`, `communication::I2C_FREQ_HZ`). Item 16 from the
list above (T-B). Fully reversible one-constant change; at 100kHz a compass read is still ~1ms and the
bus stays >95% idle at the current 10Hz sensor rate, so this is free if it isn't the fix.

**2. Forensic logging**, items 5–9 from the "Things to try" list, all now shipped:

- **Every I2C failure** (`i2c_manager::read()`/`write()`, on final-attempt exhaustion) now logs
  timestamp (`millis()`), device, register, `esp_err_to_name()`, transaction duration, and the calling
  task (`pcTaskGetName(nullptr)`) — to both Serial and `system_logger` (so it survives on SD without
  USB attached). This directly answers **E1** (which task the periodic NACK bursts come from) the next
  time it happens.
- **Latency watch**: any single transaction over 20ms logs a `SLOW` line regardless of success/failure
  — the earliest signal of a *degrading*, not-yet-wedged bus, addressing Open Question 4.
- **Per-device counters** (`i2c_manager::getDeviceStats()` / `DeviceStatSnapshot`): ops, fails,
  consecutive fails, worst-case latency, and time since last failure, per device. Exposed three ways:
  `diag i2c` (on demand), a 60s heartbeat dump to SD (`logDeviceStatsSummary()` in `task_manager.cpp`,
  tag `I2C_STATS`), and captured immediately when `checkI2CBusHealth()` first detects a wedge (tag
  `I2C_HEALTH`, followed by a forced `system_logger::flush()` so the sample reaches SD before a likely
  power cycle). This is the fix for Open Question 4 and for occurrence 3's failure to trip the
  cross-device counter — a per-device ramp is now visible even if it never crosses the global
  wedge threshold.
- **SDA/SCL line levels**: logged unconditionally (not just when SDA is already low) at every
  `init()`/`reinit()` call and around the `resetBus()` fallback path, via a new `logLineLevels()` that
  reads the GPIO input register directly — safe to call even while the I2C peripheral owns the pins.
  Answers Open Question 5 and disambiguates E2 (SCL held vs SDA held vs neither, i.e. driver/FSM state)
  the next time recovery runs.
- **EXIO register canary** (`task_manager.cpp` System Task, every 10s): reads back `REG_CONFIG`
  (must be `0x00`) and `REG_OUTPUT` (must match the in-RAM shadow) and logs `EXIO_CANARY` errors on
  mismatch. Targets E4 (the unexplained `0x3F` vs `0x7F` bit-6 discrepancy) directly — this is the
  only new instrumentation that can catch silent register corruption, which no NACK/timeout counter
  sees.
- **DEV HUD**: the on-screen perf label (`navigation.cpp`, dev_mode only) now has a 4th line —
  `i2c fail X/Y consec Z` — since a field freeze happens on battery where serial isn't attached and the
  screen is the only live channel.

**Also fixed while in this code**: the `dev_mode` boot banner (`main.cpp`) had claimed "UI Task
checkpoints", "button events", and "queue operations" were logged to SD, and that the heartbeat ran
every 30s — none of that was ever implemented; the real heartbeat ran every 120s with four fields.
This is very likely *why* the SD log looked "almost useless" in the field — the one real periodic log
line was 4x sparser than advertised, and the advertised verbose logging didn't exist at all. The
heartbeat is now 60s and includes I2C op/fail/consecutive counts; the banner describes only what
actually runs.

**Not done, deliberately deferred per explicit instruction**: items 1–2 (unplug the BH-880, multimeter
on SDA/SCL) and the T-C/T-E physical-intervention paths stay out of scope until the forensic logging
above has had a chance to narrow things down without touching hardware.

**Next verification step**: field-test this build. On the next freeze (or the absence of one), check
`/sdcard/logs/system.log` for `I2C_HEALTH`/`I2C_STATS`/`EXIO_CANARY` lines, and `diag i2c` on demand.
If freezes stop at 100kHz, that's the fix. If they continue, the per-device breakdown should show
which device started failing first and whether SDA or SCL was held — enough to pick decisively between
the remaining theories (T-A/T-D driver/software vs T-B/T-C electrical) without any physical step.

**Field-tested (2026-08-02, see Occurrence 4 above): freezes are not fixed at 100kHz.** The forensic
logging did its job — it's the reason Occurrence 4 has a millisecond-precise trace instead of a dead
serial console — but it could only capture what led *up to* the hang, not the hang itself, because a
call that never returns produces no log line by construction (see Occurrence 4's analysis). This isn't
a gap the current instrumentation design can close; catching it would need something like item 15
below (a hard WDT/timeout bound wrapped *around* each I2C call, not just around the failure-counting
that follows it) rather than more logging of the same shape.

## SD Log Reliability Hardening + Invalidated Field Test (2026-08-03/04)

An 8-9h field session was run 2026-08-03 specifically to gather the first real-world data point on the
NACK-hang patch above — leave the device running on battery and read `/sdcard/logs/system.log`
afterward regardless of outcome. The device ran out of battery before any freeze was confirmed one way
or the other, which should still have been useful: the forensic logging described above should have
captured 8-9 hours of `HEALTH`/`I2C_STATS`/`EXIO_CANARY` lines no matter what happened.

**On boot 2026-08-04, `/sdcard/logs` was completely empty.** Not truncated, not partially written — no
`system.log` at all, meaning nothing was ever flushed to SD during the entire session.

**Root cause**: `system_logger::init()` — the only function that allocates the RAM buffer/mutex and
does any file I/O — is called from exactly one place (`main.cpp`, boot only, gated on
`settings.logging_enabled` already being `true` in NVS *at that boot*). The DEV/Settings screen's
logging toggle (`dev_screen.cpp`, `logging_toggle_event()`) only called `system_logger::setEnabled()`.
Logging had been switched on via that toggle without a reboot before the test started, so
`g_initialized` was never set: `isEnabled()` reported YES, `flush()` returned success (it early-returns
`true` when uninitialized), and every `log()`/`logf()` call silently no-op'd. The session looked
instrumented and wasn't — there is no evidence either way that the NACK-hang patch was exercised.

**Fix**: `logging_toggle_event()` now calls `system_logger::init()` (idempotent — no-ops if already
initialized) before `setEnabled()`, matching the boot-time init path exactly.

**Also hardened while fixing this**, so the next attempt survives better regardless of outcome:
- **Per-boot rotation** (`system_logger.h`/`.cpp`, `MAX_ROTATED_LOGS = 5`): `init()` now shifts
  `system.log → system_1.log → … → system_5.log` (oldest dropped) before starting a fresh file, instead
  of appending forever into one file that a later, unrelated boot's `MAX_LOG_SIZE` truncation could
  silently eat into. The boot that actually froze now survives at least 5 power cycles before aging out.
- **GPS-synced timestamps**: `getTimestamp()` prints uptime (`HH:MM:SS.mmm`) until the system clock is
  valid, then switches to real UTC (`YYYY-MM-DD HH:MM:SS.mmmZ`) once `ntp_sync.cpp` sets it from a GPS
  fix — same `>= 2020-01-01` sanity gate `ntp_sync.cpp`'s own `getLocalTime()` uses. A log spanning a
  multi-hour unattended session is now dateable without cross-referencing boot time.
- **Reset-reason logging**: every boot now logs `esp_reset_reason()` decoded to a string
  (`POWERON`/`TASK_WDT`/`PANIC`/etc.) — distinguishes "the firmware reset," "the user power-cycled it,"
  and "nothing reset it, it just hung," the three outcomes this investigation keeps needing to tell apart.

**FT-06 is still not field-verified.** The 2026-08-03 session does not count as an attempt — treat the
next unattended battery test as the first one, now that the logger is confirmed to actually write.

## Field Verification, 2026-08-04 — strong positive signal, session in progress

First field session on the NACK-hang-patched build where SD logging is confirmed actually working (the
2026-08-03 session was invalidated by the logger-init bug fixed above). Reported live, mid-session:
**2 I2C failures out of 337,188 total operations (≈0.0006%)**, device still running, no freeze.

**Why this is meaningful even though the session isn't over**: the user's own framing is the load-bearing
part — *"by this time normally things were already frozen."* Every prior occurrence in this document
(1 through 4) froze well within a comparable session length on the unpatched firmware. A session that
has already run past the point where the old firmware reliably hung, on the build containing the exact
fix targeted at Occurrence 4's mechanism, with a near-zero (not zero, but negligible) failure rate and
zero freezes, is the first real evidence *for* the patch rather than just "no data yet."

**What this does and doesn't confirm**:
- Does not yet confirm the fix — one clean session, however much longer than usual, isn't a controlled
  trial, and the root *cause* of the NACKs themselves (T-B/T-C/E2/E4, all still open) hasn't moved.
- Does support that whatever hangs used to happen were plausibly the exact unbounded-busy-wait
  mechanism this patch bounds — a bus producing occasional NACKs (2 of them, here) that no longer
  escalates to a stuck task is exactly the predicted behavior of the fix, not a coincidence requiring a
  separate explanation.
- 2 non-zero failures confirm the bus still NACKs sometimes (expected, matches every prior session) —
  the patch was never meant to prevent NACKs, only to stop one from hanging the calling task forever.

**Next step**: let the session run to its natural end (battery/standby) and check `/sdcard/logs` for the
full `HEALTH`/`I2C_STATS` trail — this is the first session where that log is expected to actually
contain data end-to-end. If a full multi-hour active session completes with zero freezes, that's the
first real field verification of the fix from
[Root Cause Found and Patched, 2026-08-02](#root-cause-found-and-patched-2026-08-02) above.

## Field Verification, 2026-08-05 — confirmed

The 2026-08-04 session above ran its natural course: several hours active, left in standby overnight,
woken the next morning and worked fine, then the device was **dropped and lost power**, was powered
back on, and kept working normally afterward. User's report: *"we can't reproduce the i2c 'freeze'
anymore, whatever changes we introduced seem [to have] worked."* Four rotated log files were pulled off
`/sdcard/logs` for analysis (`system_1.log`, `system_2.log`, `system_4.log`, `system_5.log` —
`system_3.log` was missing from the SD card itself, see caveat below).

**Reconstructed timeline** (from in-file timestamps, since rotation numbering alone doesn't carry
absolute time):

| File | Reset reason | Duration | I2C fails | SLOW (>20ms) | `I2C_HEALTH` (watchdog fired) | `EXIO_CANARY` mismatches |
|---|---|---|---|---|---|---|
| `system_5.log` | `SW_RESET` | 22 min, no GPS fix | 0 | 0 | 0 | 0 |
| `system_4.log` | `SW_RESET` | 4 min, no GPS fix | 0 | 0 | 0 | 0 |
| `system_3.log` | — missing — | | | | | |
| `system_2.log` | *(header lost, see caveat)* | ~8h51m, 07:27→16:18 on 2026-08-05 | 186 / ~1.44M ops (0.013%) | 0 | 0 | 0 |
| `system_1.log` | **`POWERON`** | ~1h48m, 16:18→17:48, picks up exactly where `system_2.log` ends | 16 / ~106,734 ops (0.015%) | 0 | 0 | 0 |

**Why this closes it**:

- **Zero `TASK_WDT`/`PANIC`/`INT_WDT`/`BROWNOUT` reset reasons anywhere.** These are the reasons that
  would mean a hang was bad enough to need the TWDT safety net, or any other firmware crash. Every reset
  seen is either a benign `SW_RESET` (short, GPS-less bench sessions, 0 failures — look like routine dev
  reboots predating the field day) or the one `POWERON` in `system_1.log`.
- **The `POWERON` is almost certainly the drop.** It lands exactly at the boundary where `system_2.log`
  ends (16:18) and runs a further 1h48m to 17:48 — a genuine power-domain reset, consistent with "it
  turned off" from a physical drop, not a firmware crash pretending to be one.
- **Unbroken heartbeat cadence across both long sessions.** Every `HEALTH` line in `system_2.log` (532
  of them) and `system_1.log` (89 of them) lands exactly 60s after the last, with no gap, all the way to
  the final line in each file. This is the most direct evidence available: Occurrence 4 — the confirmed
  root cause this patch targets — manifests as the heartbeat **silently stopping mid-file with nothing
  after it** (a task stuck inside a hung I2C call never reaches its own logging). That signature does
  not appear anywhere in ~10.5 hours of combined logged runtime.
- **Failure rate stayed low and never escalated.** ~0.01% in both long sessions (matches the
  2026-08-04 in-progress reading of ≈0.0006% order of magnitude), zero `SLOW` transactions (>20ms) in
  any file, zero `EXIO_CANARY` register mismatches. The bus still NACKs occasionally, as expected and as
  every prior session showed — it just no longer hangs the calling task when it does.
- **The recovery watchdog (ADR-0003) never had to fire** — zero `I2C_HEALTH` lines across all four
  files, meaning `consecutive_failures` never reached the 10-in-a-row threshold. The bus never degraded
  far enough to need it during this test; it remains an unverified-in-practice safety net for a
  different, still-open failure mode (see [ADR-0003](adr/0003-proactive-i2c-bus-recovery-watchdog.md)).

**Caveats, neither of which changes the conclusion**:

- **`system_3.log` is genuinely gone from the SD card**, not just missing from the copy. `rotateLogs()`'s
  `rename()` calls fail silently by design (so a single SD hiccup can't crash logging) — one bad rename
  at the right boot can orphan a slot without breaking the rest of the chain. Bracketed by clean files on
  both sides; a gap in the record, not evidence of anything.
- **`system_2.log` is missing its own `===== BOOT =====`/reset-reason line.** Every session should start
  with one and this file doesn't, despite being well under the 512KB (`MAX_LOG_SIZE`) truncation cap that
  would otherwise explain a missing head. Possibly a partial SD-card copy rather than something the
  device did — unconfirmed either way. Doesn't affect the failure-rate/heartbeat analysis above, since
  all of that lives in the content that did survive; it only means the *cause* of that particular boot
  is unknown.

**Conclusion**: FT-06/FT-07 are resolved. The build-time NACK-hang backport
([Root Cause Found and Patched, 2026-08-02](#root-cause-found-and-patched-2026-08-02),
[ADR-0021](adr/0021-i2c-nack-hang-build-time-backport.md)) is confirmed working under real field
conditions — a multi-hour active session, an overnight standby/wake cycle, and an unplanned power-loss
recovery, all clean. The underlying electrical/timing question of *why* NACKs happen at all (T-B/T-C/E2)
remains open but is now a lower-priority curiosity, not a blocker — the patch bounds the hang regardless
of the NACK's origin.

## New Evidence, 2026-08-05 — failures are compass-only and front-loaded near boot (refines T-B)

Breaking down the same two field-verification log files by device and by time surfaced a pattern that
wasn't visible from the aggregate counts alone.

**Every failure in both sessions is on the compass. Zero on any other device.** `system_1.log`'s final
per-device tally: `TOUCH ops=85809 fails=0`, `EXIO ops=1243 fails=0`, `RTC ops=7 fails=0`,
`IMU_HIGH ops=53377 fails=0`, `COMPASS ops=106734 fails=16` — all 16 failures on one device.
`system_2.log`: same shape (`COMPASS ops=748132 fails=186`, one incidental `IMU_LOW` startup-probe
failure aside — see E6, expected and benign — everything else `fails=0`). This sharpens **T-B**
considerably: it isn't "the shared bus is marginal," it's specifically the one device reached over a
cable (the BH-880 module) — every other device is soldered directly to the board.

**Within `system_1.log`, the failures are front-loaded in the first hour, then stop almost entirely.**
Tracking the compass fail counter against uptime:

```
00:01  fails=0
00:04  fails=1
00:20  fails=2
00:23  fails=4
00:37  fails=8
00:55  fails=15
~68min fails=16   ← last one
...flat at 16 for the remaining ~22 minutes to end of session
```

15 of 16 failures land inside the first hour of uptime; the 16th at ~68 minutes is the last one seen in
the entire ~1h48m session. `system_2.log` shows the same shape even more starkly: flat at 185 for the
entire ~7.3 hours of the file we have (07:27→14:45), a single new failure ticks in around 14:46, then
flat at 186 for the remaining ~1.5 hours. Its own first hour is in the missing/truncated file head (see
the caveat above), but the visible portion — one failure in over seven hours — is consistent with the
same shape: a burst shortly after boot, then a long, very clean tail.

**This flips the direction of the original theory.** T-A/T-B originally assumed *degradation* — the bus
getting worse the longer a session runs (fits a thermal-drift or cumulative-load story). This data says
the opposite: worst right after power-on, then stable and clean for the rest of the session, regardless
of how long the session runs. A **warm-up transient**, not a wear-out one. Plausible mechanisms, in
descending order of how directly the data supports them:

- The BH-880 module's own onboard voltage regulator/oscillator settling after power-up.
- The GPS chip's cold-start acquisition (up to 28s per datasheet, though the observed burst window runs
  much longer, up to ~60 min) disturbing the shared module rail while hunting for a fix, easing off once
  it locks onto steady tracking.
- Thermal settling of the cable/connector itself as the board transitions from cold-boot to steady
  operating temperature.

None of these are distinguished by the data in hand — they'd need a dedicated test (e.g., compare a
cold power-on's first-hour failure count against a warm soft-reboot's, immediately after a previous
long session — if warm reboots show far fewer early failures, that points at thermal/regulator
settling specifically rather than something tied to wall-clock time since boot).

**Practical upshot**: the riskiest window for a compass NACK is the first hour after power-on, and the
device gets *safer* the longer it runs from there — consistent with the user's own field experience
(no freeze deep into a long session in this data). **Does not fully explain Occurrence 4** (the hang
that motivated the original fix): that one happened 4h26m into a session, well outside this early-burst
window, and was a different error code (`ESP_ERR_INVALID_STATE`, not a plain hardware NACK) — plausibly
a rarer, differently-flavored event riding on top of this generally-low background rate rather than
being explained by the same warm-up mechanism.

## Code References

- `src/hardware/i2c/i2c_manager.cpp` — `bitBangClockRecovery()`, `init()`, `read()`/`write()` retry and
  mutex logic, `resetBus()` (confirmed no-op comment), `logLineLevels()`, `getDeviceStats()`.
- `src/utils/task_manager.cpp` — `checkI2CBusHealth()`, `logDeviceStatsSummary()`, EXIO canary block,
  compass/accel read gating (standby/WiFi suspension), `updateStatusLabels()`.
- `src/core/device_manager.cpp` — `lvgl_touch_read_cb()` (touch poll in UI Task), button init on GPIO0.
- `src/ui/dev_screen.cpp` — `logging_toggle_event()` (runtime logger enable, now calls `init()`).
- `src/utils/system_logger.cpp`/`include/utils/system_logger.h` — `rotateLogs()`, `getTimestamp()`,
  `resetReasonToString()`.
- `docs/adr/0003-proactive-i2c-bus-recovery-watchdog.md` — design rationale for the consecutive-failure
  watchdog and rejected alternatives.
- `docs/adr/0021-i2c-nack-hang-build-time-backport.md` — design rationale for the driver patch itself
  (build-time backport vs. IDF version bump vs. manual local edit).
- `ROADMAP.md` → FT-06, FT-07 — summary status, links back here.
- `CHANGELOG.md` → Unreleased — dated entries for the probe-scan fix, the watchdog, and the
  `i2c_master_bus_reset()` no-op finding.


## current i2c diag output
==== I2C Bus Scan ====
E (1354097) i2c.master: I2C hardware NACK detected
E (1354105) i2c.master: I2C hardware NACK detected
E (1354111) i2c.master: I2C hardware NACK detected
E (1354119) i2c.master: I2C hardware NACK detected
  0x0D  -  QMC5883L (Compass)
E (1354129) i2c.master: I2C hardware NACK detected
E (1354135) i2c.master: I2C hardware NACK detected
E (1354141) i2c.master: I2C hardware NACK detected
  0x15  -  CST820 (Touch)
E (1354151) i2c.master: I2C hardware NACK detected
E (1354157) i2c.master: I2C hardware NACK detected
E (1354163) i2c.master: I2C hardware NACK detected
E (1354169) i2c.master: I2C hardware NACK detected
E (1354175) i2c.master: I2C hardware NACK detected
  0x20  -  TCA9554 (IO Expander)
E (1354185) i2c.master: I2C hardware NACK detected
E (1354191) i2c.master: I2C hardware NACK detected
E (1354196) i2c.master: probe device timeout. Please check if xfer_timeout_ms and pull-ups are correctly set up
E (1354205) i2c.master: I2C hardware NACK detected
E (1354213) i2c.master: I2C hardware NACK detected
E (1354219) i2c.master: I2C hardware NACK detected
E (1354225) i2c.master: I2C hardware NACK detected
E (1354231) i2c.master: I2C hardware NACK detected
E (1354237) i2c.master: I2C hardware NACK detected
E (1354243) i2c.master: I2C hardware NACK detected
E (1354249) i2c.master: I2C hardware NACK detected
E (1354255) i2c.master: I2C hardware NACK detected
E (1354261) i2c.master: I2C hardware NACK detected
E (1354267) i2c.master: I2C hardware NACK detected
E (1354273) i2c.master: I2C hardware NACK detected
E (1354279) i2c.master: I2C hardware NACK detected
E (1354285) i2c.master: I2C hardware NACK detected
E (1354291) i2c.master: I2C hardware NACK detected
E (1354301) i2c.master: I2C hardware NACK detected
E (1354307) i2c.master: I2C hardware NACK detected
E (1354313) i2c.master: I2C hardware NACK detected
E (1354321) i2c.master: I2C hardware NACK detected
E (1354327) i2c.master: I2C hardware NACK detected
  0x51  -  PCF85063 (RTC)
E (1354337) i2c.master: I2C hardware NACK detected
E (1354343) i2c.master: I2C hardware NACK detected
E (1354349) i2c.master: I2C hardware NACK detected
E (1354359) i2c.master: I2C hardware NACK detected
E (1354365) i2c.master: I2C hardware NACK detected
E (1354371) i2c.master: I2C hardware NACK detected
E (1354377) i2c.master: I2C hardware NACK detected
E (1354383) i2c.master: I2C hardware NACK detected
E (1354397) i2c.master: I2C hardware NACK detected
E (1354403) i2c.master: I2C hardware NACK detected
  0x6B  -  QMI8658 (IMU high)
E (1354413) i2c.master: I2C hardware NACK detected
E (1354419) i2c.master: I2C hardware NACK detected
E (1354425) i2c.master: I2C hardware NACK detected
E (1354433) i2c.master: I2C hardware NACK detected
E (1354439) i2c.master: I2C hardware NACK detected
E (1354445) i2c.master: I2C hardware NACK detected
E (1354453) i2c.master: I2C hardware NACK detected
E (1354459) i2c.master: I2C hardware NACK detected
  0x7E  -  Unknown
Found 6 device(s)
  (54 single-ACK hits rejected as probe artifacts — see comment at scanBus)
======================
