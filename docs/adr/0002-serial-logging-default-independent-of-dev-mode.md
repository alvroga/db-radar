# ADR-0002: Serial logging defaults ON, independent of `dev_mode`

Status: Accepted
Date: 2026-07-31
Decided by: Claude (proposed, you approved)

## Context

§3.5 of the performance backlog added `SerialClass::setLogEnabled(bool)` — a global gate checked
before formatting in every `print`/`printf` overload — plus a `serial on|off` console command to
toggle it, defaulting ON. Once verified working on hardware, the natural follow-up question was
whether that default should instead track `dev_mode`: this project already has a "normal mode: all
resources for radar function, no dev overhead" principle, applied
elsewhere to the on-screen DEV/perf HUD labels and the beacon proximity test path
(`task_manager.cpp:1538`).

Two things ruled it out on inspection rather than judgment call:

- **No hot-path cost exists to save.** Grepping the render path (`navigation.cpp`'s radar draw,
  `updateRadarDisplay()`) turns up zero `Serial` calls. All ~326 call sites project-wide are one-shot
  events — boot phases, task start/stop, mode transitions, errors, health-recovery attempts — not
  counted against the 10 Hz frame budget the render-pipeline work spent so much effort protecting.
  The CPU-cost argument that justifies gating the HUD/beacon-test overhead by `dev_mode` doesn't apply
  here: there's nothing recurring to gate.
- **Boot logging happens before `dev_mode` is even known.** Settings load from NVS at boot Phase 5
  (`main.cpp:62`), well after `Serial.begin()` and the Phase 1–4 boot log lines. Tying the *default*
  to `dev_mode` would only be able to take effect retroactively after settings load — and boot output
  is exactly the log stream most worth keeping visible by default when something goes wrong.

## Decision

Leave the default as shipped in §3.5: `s_log_enabled = true` at boot, unconditionally, with no link
to `settings.dev_mode`. `serial off`/`serial on` remains a manual, session-local override for anyone
who wants to silence output (e.g. during a CPU-timing-sensitive test) without losing default
visibility otherwise.

## Consequences

**Easier**: boot diagnostics and in-field error/recovery logs stay visible over USB by default,
regardless of whether `dev_mode` is toggled on — the two concerns (on-screen dev overlays vs. console
output) stay decoupled, matching how differently they're triggered (`dev_mode` is a persisted NVS
setting; serial logging only matters at all once someone has deliberately attached a monitor over
USB).

**Harder**: nothing — this is the null decision. `dev_mode` continues to gate exactly what it gated
before (HUD/perf labels, beacon proximity test features), and serial logging continues to be
independently controllable.

**Gave up**: none. The one real cost this could have removed — log lines executing (format + a
fail-fast dropped `cdcacm_write`) when no monitor is attached in the field/battery scenario — is
unaffected by *default* state either way, since `serial off` was already available for anyone who
wants it and dev_mode-gating wouldn't reach the pre-settings-load boot sequence regardless.
