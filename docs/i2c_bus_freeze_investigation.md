# I2C Bus Freeze Investigation (FT-06 / FT-07)

**Status**: **Resolved and field-verified, 2026-08-05.** Root cause found and patched 2026-08-02 — a
real ESP-IDF driver bug (confirmed against Espressif's own upstream issue tracker and source history),
not application logic. **The `i2c_master.c` NACK-handling busy-wait had no timeout bound at all** and
is patched at build time via `scripts/patch_i2c_master_nack_hang.py`. `panic_on_timeout` (TWDT) stays
`true` as a belt-and-suspenders safety net in case any other, still-unknown hang mechanism exists. See
[Root Cause and Fix](#root-cause-and-fix-patched-2026-08-02) and
[Field Verification](#field-verification-2026-08-05--confirmed).

**Kept open, lower priority**: the *why* behind the NACKs themselves is still unconfirmed. It no
longer matters for the freeze itself — the patch bounds the hang regardless of why a NACK happens —
but would explain why this bus produces NACKs at all. See [Open Questions](#open-questions).

This document originally consolidated a multi-week investigation (2026-07-31 to 2026-08-05) across
four occurrences, several rejected recovery mechanisms, and two rounds of forensic-logging builds.
That process log has been trimmed out (recoverable from git history as of 2026-08-07) now that the
bug is closed and [ADR-0021](adr/0021-i2c-nack-hang-build-time-backport.md) captures the fix decision;
what remains here is the mechanism detail code comments point back to, and the still-open questions.

---

## Root Cause and Fix, Patched 2026-08-02

`components/esp_driver_i2c/i2c_master.c`, function `s_i2c_send_commands()` (pinned ESP-IDF **5.5.0**).
After a transaction's event queue reports `I2C_EVENT_NACK`, the driver waits for the hardware to
finish the STOP condition with:

```c
} else if (event == I2C_EVENT_NACK) {
    while (i2c_ll_is_bus_busy(hal->dev)) {
        __asm__ __volatile__("nop");
    }
}
```

**No bound of any kind.** If the hardware doesn't autonomously complete the STOP (a slave stretching
SCL, or the I2C FSM getting stuck post-NACK), the calling FreeRTOS task spins here **forever**,
completely bypassing the timeout the application passed to `i2c_master_transmit_receive()`. Reachable
from every transaction `i2c_manager::read()`/`write()` makes.

Confirmed as a known, already-fixed upstream bug:
[espressif/esp-idf#17720](https://github.com/espressif/esp-idf/issues/17720) — same function, same
loop, same backtrace, independently reported on different hardware. Fixed in v5.5.4 (bounded wait +
`s_i2c_hw_fsm_reset()` on expiry); still broken in v5.5.1–v5.5.3. This project is pinned to 5.5.0;
PlatformIO's registry tops out at IDF 5.5.3 for the 5.5.x line (still broken), and the next available
version is IDF 6.0.x (a major-version jump not proportionate to one driver bug).

**The fix**: `scripts/patch_i2c_master_nack_hang.py`, wired into `platformio.ini`'s `extra_scripts`,
backports just the timeout-bound block from upstream v5.5.4 into the pinned framework's
`i2c_master.c` at build time — idempotent (no-ops if already patched, refuses and warns loudly if
neither the old nor new block is found rather than guessing). Full decision rationale, including the
two rejected alternatives (bump IDF to 6.0.x; manually patch the installed package file), is in
[ADR-0021](adr/0021-i2c-nack-hang-build-time-backport.md).

**What this does and doesn't close**: closes the specific hang mechanism — a NACK-triggered spin with
no timeout, now bounded and self-recovering. Does **not** close *why* the bus produces NACKs the
hardware can't autonomously recover from in the first place (see Open Questions), and doesn't rule out
a second, different hang mechanism existing elsewhere in the driver — that's what `panic_on_timeout`
remains for.

## Field Verification, 2026-08-05 — Confirmed

Four rotated SD log files (`system_1.log`, `system_2.log`, `system_4.log`, `system_5.log` —
`system_3.log` genuinely missing from the SD card, a silent-by-design `rename()` failure unrelated to
this bug) were pulled after a real-world session: several hours active, an overnight standby/wake
cycle, then the device was **dropped and lost power**, powered back on, and kept working normally.

| File | Reset reason | Duration | I2C fails | `SLOW` (>20ms) | Watchdog fired | `EXIO_CANARY` mismatches |
|---|---|---|---|---|---|---|
| `system_5.log` | `SW_RESET` | 22 min | 0 | 0 | 0 | 0 |
| `system_4.log` | `SW_RESET` | 4 min | 0 | 0 | 0 | 0 |
| `system_2.log` | *(head lost)* | ~8h51m | 186 / ~1.44M ops (0.013%) | 0 | 0 | 0 |
| `system_1.log` | **`POWERON`** | ~1h48m | 16 / ~106,734 ops (0.015%) | 0 | 0 | 0 |

**Why this closes it**: zero `TASK_WDT`/`PANIC`/`INT_WDT`/`BROWNOUT` reset reasons anywhere — every
reset is a benign `SW_RESET` or the one `POWERON`, which lands exactly where `system_2.log` ends and
matches "it turned off" from the drop, not a firmware crash. The heartbeat cadence (532 lines in
`system_2.log`, 89 in `system_1.log`) never has a gap — the freeze this patch targets manifests as the
heartbeat silently stopping mid-file, and that signature does not appear anywhere in ~10.5 hours of
combined logged runtime. Failure rate stayed low (~0.01%) and never escalated; the recovery watchdog
([ADR-0003](adr/0003-proactive-i2c-bus-recovery-watchdog.md)) never had to fire.

Getting to a *trustworthy* field test took two prerequisite fixes, both still live in the code:
- **Per-boot log rotation** (`system_logger.cpp`, `MAX_ROTATED_LOGS = 5`) — the boot that actually
  froze needs to survive several power cycles before aging out of the single-file log.
- **Logging must be `init()`'d, not just `setEnabled()`'d, from the DEV screen toggle** — an earlier
  8-9h field session (2026-08-03) silently logged nothing because the toggle only flipped an enabled
  flag without allocating the logger's buffer/mutex; `logging_toggle_event()` now calls the same
  `init()` the boot path uses.

## Open Questions

**Why does this bus produce NACKs at all?** Breaking down the two long 2026-08-05 sessions by device
and by time surfaced a clear pattern: **every failure in both sessions is on the compass, zero on any
other device** (`COMPASS ops=748132 fails=186` in one file, `ops=106734 fails=16` in the other — TOUCH,
EXIO, RTC, IMU all `fails=0`). This points specifically at the one device reached over a cable (the
BH-880 module) rather than "the shared bus is marginal" generally.

**And they're front-loaded near boot, not accumulating over the session** — 15 of 16 failures in one
file land inside the first hour of uptime, then flat for the remaining ~22 minutes; the other file
shows one failure in its visible first ~7.3 hours, then a single new one, then flat. This flips the
original assumption (gradual degradation, a thermal-drift/cumulative-load story) — the data says
**worst right after power-on, stable and clean for the rest of the session**, a warm-up transient, not
a wear-out one. Candidate mechanisms, untested: the BH-880's own regulator/oscillator settling after
power-up, GPS cold-start acquisition disturbing the shared module rail, or thermal settling of the
cable/connector. None distinguished by data in hand.

**Doesn't explain everything**: the original hang this patch fixes happened 4h26m into a session, well
outside the early-burst window, with a different error code (`ESP_ERR_INVALID_STATE`, not a plain
NACK) — plausibly a rarer, differently-flavored event riding on top of this generally-low background
rate.

**Also unconfirmed**: whether the EXIO chip (TCA9554) can reach a state that clock pulses alone can't
clear (a genuine power-on-reset need, not just a bus-interface reset) — no occurrence has ever logged
`bitBangClockRecovery()` reporting `STILL LOW`. One diagnostic scan (below) also caught a probe
artifact surviving even a double-ACK confirmation guard at address `0x7E` (reserved I2C range, no real
device answers there) — a reminder the scan-artifact class of bug (see `scanBus()`'s comment) is still
present even on an otherwise-healthy bus.

```
==== I2C Bus Scan ====
  0x0D  -  QMC5883L (Compass)
  0x15  -  CST820 (Touch)
  0x20  -  TCA9554 (IO Expander)
  0x51  -  PCF85063 (RTC)
  0x6B  -  QMI8658 (IMU high)
  0x7E  -  Unknown   ← reserved I2C range, no real device; probe artifact survived double-ACK confirm
Found 6 device(s)
  (54 single-ACK hits rejected as probe artifacts — see comment at scanBus)
======================
```

None of the above blocks anything — the patch bounds the hang regardless of the NACK's origin. If
anyone picks the electrical/timing question back up: `docs/i2c.md`'s per-device forensic counters
(`getDeviceStats()`) and the `SLOW`/`EXIO_CANARY` log lines already give enough instrumentation to
correlate a future occurrence against cold-boot timing, GPS fix state, and battery charge state
without needing new logging first.

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
- `CHANGELOG.md` → dated entries for the probe-scan fix, the watchdog, and the
  `i2c_master_bus_reset()` no-op finding.
