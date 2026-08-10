# ADR-0003: Proactive I2C bus-recovery watchdog, not per-failure or task-hang-triggered recovery

Status: Accepted
Date: 2026-07-31
Decided by: Claude (proposed, you approved)

## Context

A full interface freeze (button, touchscreen, display updates all unresponsive) happened twice in one
session. The first time needed a full power cycle; the second cleared on its own when the device went
to standby and woke back up. That second occurrence is diagnostic: wake-from-standby calls
`i2c_manager::reinit()` (9 SCL clock-recovery pulses + full bus rebuild) as part of its own recovery
sequence, and that is what cleared touch/button/sound. The root cause is a stuck I2C bus — a slave
(CST820 touch, PCF85063 RTC, or TCA9554 EXIO) left mid-transaction holds SDA low indefinitely, and an
MCU-only reset cannot free an external chip, so the bus stays jammed across a reset until it is
explicitly clock-recovered.

`reinit()` already existed and was already proven to work — the question was only ever *when to call
it automatically*, without a person needing to trigger standby or a power cycle by hand. Three other
designs were considered and rejected:

- **Call `reinit()` on every single I2C failure.** Rejected: transient single-transaction failures are
  normal and already handled by `read()`/`write()`'s existing 3x retry — reinit-ing the whole bus on
  every one of those would tear down and rebuild all six device handles far more often than needed,
  including during ordinary operation, for no benefit over letting the existing retry succeed.
- **Fold recovery into the existing generic task-hang recovery (`attemptTaskRecovery()`,
  `task_manager.cpp`).** That function already exists and reacts to a task's loop counter stalling —
  but it only suspends/resumes the FreeRTOS task handle. It doesn't touch the I2C bus at all, so if the
  root cause is a wedged bus, suspend/resume is a no-op: the task resumes and immediately fails the
  same I2C calls again. Extending it to also call `i2c_manager::reinit()` was considered, but task-hang
  detection fires on a completely different signal (a stalled loop counter, checked every 5s) than bus
  health does — coupling them would mean the I2C fix only ever fires after a task has already been
  visibly stuck long enough to trip that separate check, rather than as soon as the bus itself shows
  the failure pattern.
- **Do nothing further and rely on standby-wake / manual power cycle.** This is the status quo the
  incident exposed as insufficient — it requires a person to notice a fully frozen device and act.

## Decision

Add a dedicated, bus-level signal — `i2c_manager::Stats::consecutive_failures`, incrementing on every
failed `read()`/`write()` and resetting to 0 on the next success — and a small watchdog
(`checkI2CBusHealth()`) in System Task that watches it directly, independent of any task-hang
detection. At ≥10 consecutive failures (chosen against touch's ~11.7 Hz poll rate: under 1 second of a
*fully* jammed bus, since a wedged bus fails every transaction to every address, not just one device's
occasional miss) it calls `i2c_manager::reinit()` — the same call already proven to work via
standby-wake. A 2-second cooldown between attempts and a 5-attempt cap (logging `UNRECOVERABLE` and
stopping, mirroring the existing pattern in `attemptTaskRecovery()`) prevent it from fighting genuinely
dead hardware forever.

Separately, `i2c_manager::init()` was also hardened: if the initial boot-time EXIO ping fails, it now
retries once after a clock-recovery pulse instead of just warning and continuing. This covers the
already-documented boot-hang case (`docs/troubleshooting.md`) and any reboot that follows a runtime
wedge, which previously came back up still jammed.

## Consequences

**Easier**: the device can now self-heal from this failure mode without a person noticing a frozen
screen and manually triggering standby or a power cycle. The signal used (consecutive failures) is
bus-level and independent of which task happens to be calling I2C at the time, so it isn't tied to any
particular task's hang-detection cadence.

**Harder**: nothing structural — the watchdog is a small, isolated addition (RAM +8 B, flash +868 B)
that reuses an existing, already-tested recovery primitive rather than introducing a new one.

**Gave up / open risk**: this fix has **not been verified against a real bus wedge** — there is no
safe way to deliberately jam the I2C bus to test it on demand, so its effectiveness can only be
confirmed by watching for the recovery log lines (`[I2C] ... attempting recovery`) if the freeze
recurs in the field. If it turns out the threshold (10) or cooldown (2s) need tuning — e.g., the freeze
recurs but self-clears slower or faster than expected — that should be adjusted from real log data,
not re-derived from the same reasoning that produced these numbers the first time.

A second, unrelated bug was found alongside this one (the on-screen DEV/perf HUD label staying frozen
after an otherwise-successful recovery) and is explicitly **not** addressed by this decision — see
`docs/performance_optimization_backlog.md` → work queue → "Open bugs" for that follow-up.

## Update, 2026-08-05

FT-06 is now resolved — see [ADR-0021](0021-i2c-nack-hang-build-time-backport.md) and
`docs/i2c_bus_freeze_investigation.md`, "Field Verification, 2026-08-05". The actual fix was a
different mechanism than this ADR anticipated: not a wedged bus needing bus-level recovery, but a
single I2C call inside the driver itself hanging forever past its own timeout (an ESP-IDF bug,
`i2c_master.c`'s unbounded NACK-wait), patched by bounding that wait, not by this watchdog.

This watchdog **never fired** during the ~10.5h field verification — zero `I2C_HEALTH` log lines,
`consecutive_failures` never reached 10. That's consistent with the two fixes targeting different
failure modes: this ADR's mechanism (a fully wedged bus, SDA stuck low) never occurred in that test, so
"gave up / open risk" above — never verified against a real bus wedge — is still true today. The
watchdog remains in place as a safety net for that separate, still-hypothetical scenario; it did not
need to be, and was not, the thing that closed FT-06.
