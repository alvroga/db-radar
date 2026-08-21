# ADR-0034: Touch NACK handling — throttled retry + DisAutoSleep, not a permanent circuit breaker

Status: Accepted
Date: 2026-08-21
Decided by: Claude (proposed, user approved via field verification)

## Context

Two boards (the user's second unit and an independent field report) showed the same pattern: the
CST820 touch controller works briefly — right after a flash, or for ~1-2s after a TP_RST pulse — then
NACKs every I2C transaction indefinitely. Two prior fixes (`cde07d3`) had stopped the *collateral
damage* (a NACK cascade crashing the board; Core-1 starvation hanging boot) but left touch itself
dead, and the leading hypothesis had drifted to failing hardware (marginal FPC connector).

Root cause, found 2026-08-21: some CST8xx chip firmware batches auto-enter standby after ~1-2s
without a touch and **NACK all I2C while asleep** — indistinguishable on the bus from a dead chip.
Only a finger or TP_RST wakes them. Waveshare's own demo writes register `0xFE` (DisAutoSleep) after
reset; this project never did, and the original board's chip merely happened not to sleep
aggressively. Full evidence chain: `docs/touch.md`'s Auto-Sleep section.

Three interacting mechanisms had to change, and each had a plausible-but-wrong alternative.

## Decision

1. **Write DisAutoSleep (`0xFE`) whenever the chip is known-awake**: end of `initTouch()`, after
   standby-wake's TP_RST, and on recovery inside the read callback. The register is volatile, so a
   single init-time write is not sufficient — every chip-reset path needs a re-arm.
2. **The read-failure circuit breaker throttles to 250ms instead of permanently disabling polling**
   after 10 consecutive failures, resuming full rate (and re-arming DisAutoSleep) on the first
   successful read.
3. **The I2C bus-wedge detector requires consecutive failures from ≥2 distinct devices** (each with
   `consecutive_fails ≥ 2`) on top of the global 10-failure threshold before triggering a full bus
   reinit.

## Alternatives rejected

- **Keep the permanent breaker (status quo)**: guaranteed permanent touch death on any board with a
  sleepy chip — the breaker trips ~1s after boot while the chip is merely asleep, and the only wake
  path (a finger press being polled) is exactly what it turned off. This is why touch was dead all
  session on `cde07d3`-era builds while a pre-breaker build's first-boot picker still accepted taps.
- **Remove the breaker entirely**: reintroduces the original NACK-cascade cost on a genuinely dead
  chip — ~9ms of bus occupancy per failed read at LVGL's poll rate, contending with RTC/EXIO/
  compass/IMU traffic, which is what produced the crash/reboot loop the breaker was built to stop.
  250ms throttling keeps a dead chip's cost negligible (~4 failed reads/s) while a sleeping chip
  still gets woken by the next press within a retry or two (a real press spans ~130-190ms).
- **Poll-side wake writes (periodically strobe the chip over I2C)**: the chip NACKs writes while
  asleep too — there is no bus-side wake. Only touch or TP_RST wake it, so the fix must be to stop
  it sleeping, not to wake it on demand.
- **Wedge detector: keep the single global counter**: one high-rate NACKing device (touch at
  ~11-12Hz) saturates it alone in under a second, triggering full-bus reinits — and mid-transaction
  teardown risk for healthy devices — on a bus with nothing wrong. The detector's own design comment
  already said the signal was supposed to be cross-device; the implementation just never checked.
  A real wedge (slave holding SDA low) by definition fails every device, so the ≥2-device
  requirement costs nothing in detection power.

## Consequences

- Boards with auto-sleeping CST8xx batches work identically to non-sleeping ones; the two field
  reports are explained and the FPC-connector/hardware theory is retired without any hardware change.
- A genuinely dead/disconnected touch chip now costs ~4 failed I2C reads per second for the life of
  the session instead of zero after the first second. Accepted: that traffic is negligible at 100kHz,
  and it buys automatic recovery from every transient failure mode (sleep, brownout, ESD reset).
- `touch_ok` no longer latches false at runtime, so `Device Status Summary` reflects boot-time state
  only; runtime health shows in the throttle/recovery log lines instead.
- The wedge detector reads per-device stats on every check past the global threshold — a few dozen
  extra instructions, only on an already-failing bus.
