# ADR-0021: Build-time backport of the IDF NACK-hang fix, not a version bump or a manual edit

Status: Accepted
Date: 2026-08-02
Decided by: Claude (proposed, you approved)

## Context

FT-06 (recurring full interface freeze, `docs/i2c_bus_freeze_investigation.md`) traced to a real bug in
the pinned ESP-IDF 5.5.0's `i2c_master.c`: after an I2C transaction's event queue reports
`I2C_EVENT_NACK`, the driver waits for the hardware to finish the STOP condition with an unbounded
`while (i2c_ll_is_bus_busy(hal->dev)) { nop(); }` — no timeout, no bound of any kind. If the hardware
doesn't autonomously complete the STOP (a slave stretching SCL, or the I2C FSM getting stuck post-NACK),
the calling FreeRTOS task spins there forever, completely bypassing the timeout the application passed
to `i2c_master_transmit_receive()`. This matches Espressif's own upstream issue
[#17720](https://github.com/espressif/esp-idf/issues/17720), independently reported on different
hardware, same function, same backtrace.

Diffing tagged releases confirmed exactly when it was fixed upstream:

| Tag | NACK-wait loop |
|---|---|
| v5.5.1, v5.5.2, v5.5.3 | unbounded (bug present) |
| **v5.5.4** | bounded — `TickType_t start_tick` + `ticks_to_wait` check, calls `s_i2c_hw_fsm_reset()` on expiry |
| v5.5.5 (latest tagged) | same fix, present |

This project is pinned to **5.5.0** — older than even the confirmed-still-broken 5.5.1. PlatformIO's
package registry (`platformio/framework-espidf`) tops out at `3.50503.0` = IDF 5.5.3 for the 5.5.x line
— still broken. The next available version, `4.60000.0`/`4.60001.0`, is IDF **6.0.x** — a major-version
jump.

Three approaches were considered.

## Decision

Backport just the timeout-bound fix into the vendored driver **at build time**, via
`scripts/patch_i2c_master_nack_hang.py` wired into `platformio.ini`'s `extra_scripts` (the same
mechanism already used by `scripts/gen_version.py`). It locates the pinned framework's `i2c_master.c`
via `env.PioPlatform().get_package_dir("framework-espidf")` — portable, no hardcoded path — and applies
only the exact block as it landed in upstream v5.5.4, not the larger unrelated read-command-batching
refactor that shipped in the same upstream release. It is idempotent by construction: replaces the old
block only if found byte-for-byte unpatched, no-ops with a log line if already patched, and — if neither
the old nor new block is found (framework package version changed since this script was written) —
does **not** guess; it leaves the file alone and prints a loud warning instead of silently failing to
protect against a bug it no longer recognizes.

**Rejected alternatives**:

- **Bump the pinned IDF version.** The only versions PlatformIO offers that contain the fix are IDF
  6.0.x — a major-version jump with near-certain breaking API/Kconfig changes across the whole project
  (display driver, LVGL glue, NimBLE, partitions), an enormous and risky undertaking to fix one function
  in one driver file. Not proportionate to the bug.
- **Manual local edit of the installed package file.** Would work once, on one machine, until the next
  `pio pkg install`/clean checkout silently reverts it — the installed framework lives outside the repo
  and isn't tracked by git. Indistinguishable from "not fixed" on any other machine or CI, and the
  regression would be silent (no error, just the bug coming back).
- **Do nothing further, rely only on `panic_on_timeout` (TWDT).** Already enabled as a belt-and-suspenders
  safety net (see `docs/i2c_bus_freeze_investigation.md`), but it only converts a silent infinite hang
  into a 30-second-later reset — the device still visibly freezes for up to 30s and loses whatever state
  wasn't persisted. It treats the symptom, not the bug; kept as a safety net for *other*, still-unknown
  hang mechanisms, not as the fix for this one.

## Consequences

**Easier**: the fix travels with the repo — any machine or CI that runs `pio run` against the pinned
framework package gets it automatically, with no manual step and no dependency on remembering to
re-apply it after a clean install. Idempotency means re-running the build (or a CI cache miss that
reinstalls the framework) can't double-patch or corrupt the file.

**Harder**: the script is coupled to the exact byte-for-byte shape of IDF 5.5.0's `i2c_master.c`. If
PlatformIO's registry ever offers a version where this function's source has changed for unrelated
reasons, the patch will (by design) refuse to apply and print a warning rather than silently no-op —
that warning has to actually get noticed the next time someone runs a full build.

**Gave up / open risk**: this only closes the *hang* — it does not explain or fix *why* this bus
produces NACKs in the first place (the electrical/signal-integrity theories, T-B/T-C, and the
EXIO-corruption question, E4, in the investigation doc are all still open). If IDF is ever bumped past
6.0.x in the future for unrelated reasons, this patch becomes dead weight and should be deleted once the
target version is confirmed to already contain the upstream fix.

**Field-verified 2026-08-05**: ~10.5h of combined logged active runtime across two sessions, spanning an
overnight standby/wake cycle and a real power-loss recovery, zero freezes, unbroken heartbeat cadence
throughout. See `docs/i2c_bus_freeze_investigation.md`, "Field Verification, 2026-08-05 — confirmed".
