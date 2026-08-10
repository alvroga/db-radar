# ADR-0013: `I2C_PROCESS_MS = 20` is a tuned floor, not an arbitrary interval

Status: Accepted
Date: 2026-07-31
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The I2C Task also drives the buzzer's sonar state machine (`buzzer::update()` runs from this task
because it needs EXIO over I2C), so its cycle time is the sonar's timing-resolution floor. At 20ms
that floor is 7–13% jitter on a 150–300ms sonar interval — audible, and flagged in the field as "not
metronomic" (`docs/performance_optimization_backlog.md` §8.1b).

Halving `I2C_PROCESS_MS` from 20 to 10ms was tried on hardware to fix this (commit `93e864b`). The
cost analysis behind it argued the change was "almost free," but only counted the I2C Task's own CPU
— a non-blocking queue drain plus a few timestamp compares, which is indeed trivial. Field result:
**button unresponsive, buzzer silent.** Reverted immediately in commit `ca87cc3`
(`include/utils/task_manager.h:56`, `src/utils/task_manager.cpp`), and the revert was confirmed
working on hardware — radar, beacon discovery, and sound all back to normal at 20ms.

**The root cause was never diagnosed.** The revert commit and the backlog explanation both attribute
the failure to "the CST820 touch driver calls `Wire` directly, bypassing `i2c_mutex`, doubling this
task's rate doubles the collision rate against an already-contended bus." That mechanism is stale: a
grep of `src/` and `include/` for `\bWire\b` turns up no live call sites — only a vendor-driver
comment (`src/hardware/sensors/gyro_qmi8658.cpp:20`), a stub-header comment
(`include/hardware/i2c/i2c_driver.h:5`), an `arduino_compat.h` doc comment noting `Wire` is *not*
provided, and the stale explanatory comment in `task_manager.h:47` itself. In the current ESP-IDF
build, `cst820_read()` goes through `i2c_manager::read()` under `g_bus_mutex`
(`docs/compass_i2c_constraint.md`), the same path RTC, EXIO, and the compass use. So the specific
blamed mechanism does not exist in this codebase — the symptom is real and hardware-confirmed, but
its explanation is not.

## Decision

Keep `I2C_PROCESS_MS = 20` as a hard floor. Do not retry 10ms until the actual failure mechanism is
understood — a stale/incorrect explanation is not a reason to believe the fix (raising the rate again)
is safe, only a reason to distrust the earlier confidence that it was "almost free."

## Consequences

**Easier**: nobody re-spends the effort re-discovering this the hard way — the constant is marked with
a `⚠️ DO NOT LOWER THIS` comment at its definition (`task_manager.h:43`) citing the 2026-07-31 field
result, and the backlog item is marked void (§8.1b, item 22 in the effort table).

**Harder**: sonar beat steadiness has to be bought without touching the shared bus — a deadband + slew
limit on the tempo, and a median filter on RSSI before the EMA, rather than the simpler "halve the
clock" fix.

**Gave up**: the 20ms floor stays as a permanent quantization limit on sonar timing resolution for as
long as the buzzer is driven over EXIO on the shared I2C bus — not because 20ms is provably necessary,
but because the one data point that exists says lowering it breaks the device, and nobody has a causal
model good enough to say why or under what conditions it would be safe.
