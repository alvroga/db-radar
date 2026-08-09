# ADR-0016: FreeRTOS multi-task architecture instead of a single Arduino loop

Status: Accepted
Date: 2025-10 (precise day not recoverable — see Context)
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The original design ran everything from Arduino's `setup()`/`loop()` on a single core. I2C operations
(RTC reads, EXIO writes) are inherently blocking, and running them inline with LVGL's timer handler
produced roughly 100ms UI freezes every time a device was polled — bad enough that CLAUDE.md still
describes it as "the core issue" the task architecture exists to eliminate.

The fix was architectural, not a tuning fix: split the work across four FreeRTOS tasks instead of one
loop, so a blocking I2C transaction on one core cannot stall LVGL rendering on the other. The task
header itself documents this as satisfying "Priority 3.7 requirements"
(`include/utils/task_manager.h:15`), but no CHANGELOG entry names that milestone directly — the
earliest recoverable evidence is `task_manager.cpp` already being referenced for the GPS task at the
project's very first tracked release, `[v0.2.0] - 2025-10-07` ("GPS Integration (Priority 1.2)",
CHANGELOG.md). The repository's actual git history was squashed into a single "Initial public release"
commit (`9be4aa6`, 2026-05-09), so no per-commit date survives for the original task split either.
`2025-10` is therefore the most specific supportable date, not a precise day.

## Decision

Run four dedicated FreeRTOS tasks instead of one loop: **UI Task** (Core 1, highest of the four
priorities — LVGL processing and touch/button input), **I2C Task** (Core 0 — all device communication,
consumed from a request queue with callbacks, `task_manager::queueI2CRequest`), **Network Task**
(Core 0, lowest priority — WiFi/BLE scanning), and **System Task** (Core 0, lowest priority — sensor
reads, memory monitoring, diagnostics). Current constants live in `include/utils/task_manager.h:24-39`
(`TaskConfig`); the relative ordering — UI highest, background tasks low — is unchanged from the
original design even though the absolute priority numbers have since been retuned (e.g. `UI_PRIORITY`
is now 5, positioned "above FreeRTOS timer svc (1), below WiFi driver (22-23)" per the comment at
`task_manager.h:32` — CLAUDE.md's prose used to lag behind at `3`, since corrected).

## Consequences

**Easier**: the ~100ms UI freezes are gone — I2C latency on Core 0 no longer blocks LVGL rendering on
Core 1. All device communication funnels through one queue (`I2CRequest`/`queueI2CRequest`) with a
consistent retry/callback pattern instead of ad hoc blocking calls scattered through UI code.

**Harder**: **LVGL is not thread-safe, so once these tasks start, only the UI Task may call LVGL
functions.** Every other task must go through `task_manager::queueUIUpdate()` (queues a `UIUpdate` for
the UI Task to apply) or `task_manager::withDisplayMutex()` (runs a function while holding
`display_mutex`, declared `include/utils/task_manager.h:235`/`307`/`272`). The `display_mutex`
prevents *concurrent* LVGL access from a different core, but does **not** prevent *same-core*
preemption re-entrancy — the UI Task can still preempt a lower-priority task mid-LVGL-call on Core 1 if
something on that core ever calls LVGL directly, which was a real, expensive-to-diagnose bug class this
project hit at least once (project memory: "Symptom of violation: UI_Task hangs at loop count 2,
watchdog fires, health monitor shows UNRECOVERABLE"). Four tasks across two cores and priority levels
is also simply more to reason about than one loop.

**Gave up**: the simplicity of straight-line, blocking code. Every device read is now request-in,
callback-out instead of a direct function call, and cross-task UI updates require going through the
queue/mutex discipline above rather than touching a label directly.
