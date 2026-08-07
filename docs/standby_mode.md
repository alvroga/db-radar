# Standby Mode - GPS Radar

**Status**: Complete ✅

## Overview

A low-power sleep state that turns the display off and cuts WiFi/AP/CPU clock while keeping GPS
alive at reduced power, so a long field session doesn't drain the battery between waypoint checks.
Two ways in: a manual 4-second button hold, or an optional inactivity timeout. Exactly one way out:
press the button.

## Entering Standby

**Manual**: hold the GPIO0 button for **4 seconds**. `button.cpp`'s hold-detection checks the 4s
`EXTRA_LONG_PRESS` threshold *before* falling back to the 2s `LONG_PRESS` (Settings) threshold
(`s_config.extra_long_press_ms = 4000`, `long_press_ms = 2000` — `include/hardware/input/button.h`),
so a 4-second hold never opens Settings on its way past the 2-second mark.

**Automatic**: Settings > Display > Auto Sleep, a dropdown of Off / 5 / 10 / 15 / 30 minutes
(`RadarSettings::auto_sleep_timeout_minutes`, NVS key `auto_sleep`, default **0 = disabled**). The
System Task calls `standby_manager::checkInactivityTimeout()` every tick (~100ms); it compares
`millis()` against the last user-activity timestamp and queues the same `ENTER_STANDBY` update the
manual button path uses once the configured timeout elapses. "Activity" is a button press
(`button.cpp:129`) or a valid on-screen touch (`device_manager.cpp:1251`, inside the circular-display
hit test) calling `standby_manager::notifyUserActivity()`.

**On entry** (`enterStandby()`): saves current brightness/WiFi/AP state, resets zoom to 100m and
explicitly disables BLE beacon scanning (`beacon_proximity::setEnabled(false)`) — done immediately
rather than left to the zoom-gated logic, since beacon scanning only runs at 50m zoom and standby
needs it off deterministically, not incidentally. A black "STANDBY MODE" overlay then shows for
**3 seconds** (`STANDBY_SCREEN_DURATION_MS`) before actual power-down.

## What's Off, What's Not

Applied by `applyStandbyPowerSettings()` once the 3-second overlay timer fires:

| Subsystem | Standby state |
|---|---|
| Backlight | `backlight::off()` — full off, not `setPercent(0)` (see "Why `off()`, not `setPercent(0)`" below) |
| WiFi scanning | Disabled, only if it was on before entry |
| AP / web GPX manager | Stopped, only if it was on before entry |
| GPS | **Stays on**, switched to power mode `0x02` ("Aggressive 1Hz") — not full power-down, since continuous track logging is the whole point of standby existing |
| CPU frequency | 240MHz → 80MHz (`setCpuFrequencyMhz(80)`) — safe because GPS UART, I2C, and the RGB panel all run off independent clock sources |
| Touch (CST820) | **Not polled at all** — `lvgl_touch_read_cb()` returns immediately while `standby_manager::isStandby()`, because continuous I2C reads at 100Hz with the display off were found to corrupt the touch controller's state over time. This is *why* wake requires a button press specifically — touch input is physically not being read during standby, not just ignored at the UI layer. |

**Why `off()`, not `setPercent(0)`**: `backlight::setPercent()` enforces a 5% floor so a misconfigured
brightness setting can never leave the screen unreadable with no way back (see CLAUDE.md's Battery
Monitoring section for the related round-display-crop story). Standby is the one place a fully-dark
screen is the intended outcome, so it calls the explicit `off()` path that bypasses that floor.

## Waking

Any button press while `isStandby()` queues a `WAKE_STANDBY` update (never a direct call — see
Thread-Safety below). `wakeFromStandby()`:

1. Restores brightness, WiFi, and AP to their pre-standby state.
2. Destroys the standby overlay (`lv_obj_del`) — the underlying screen object was never replaced,
   only covered, so it reappears intact.
3. Resets zoom to 100m.
4. **Unconditionally navigates to the radar screen** (`navigation::goToRadarScreen()`) — regardless
   of which screen was showing when standby was entered. This is worth calling out because the
   overlay mechanism itself (see "Why an overlay, not a screen switch" below) was built specifically
   to *preserve* the prior screen, and older project history describes waking back to "the exact same
   screen (Settings or Radar)." That's no longer the actual behavior — a later change forces radar on
   every wake regardless. The overlay trick still matters for a different reason: it avoids the
   LVGL object-invalidation crash a real screen switch caused (below), it just doesn't currently
   result in returning to a non-radar screen.
5. Re-initializes the I2C bus (`i2c_manager::reinit()`) and hard-resets the CST820 touch controller
   by toggling its `TP_RST` line via the EXIO expander, then pings it to confirm it's responding.
   This exists because the ESP32 I2C controller's hardware state machine can be left stuck after the
   touch chip goes quiet for a while in standby — a soft bus recovery (SCL pulsing) alone doesn't
   clear it, only a full driver teardown/rebuild plus a physical touch-chip reset does.
6. GPS power mode restored to `0x00` (full power), CPU restored to 240MHz.

A 1200ms post-wake suppression window (`device_manager.cpp`) swallows the trailing `SINGLE_PRESS`
that the button state machine would otherwise fire after the wake press's double-press window
expires — without it, waking would also trigger an unwanted zoom change.

## Why an Overlay, Not a Screen Switch

The original design used `lv_scr_load()` to switch to a dedicated standby screen and back. That
caused a `LoadProhibited` NULL-pointer crash (0x00000020) on wake — screen switching invalidated the
radar canvas objects. The fix: `enterStandby()` creates the standby UI as a full-screen **child
object** of the currently-active screen (`lv_obj_create(current_screen)`) instead of loading a
separate screen, and `wakeFromStandby()` simply deletes that child. The parent screen's object tree
is never touched, so there's nothing to invalidate.

## Thread-Safety: Why Everything Is Queued

`enterStandby()`/`wakeFromStandby()` make LVGL calls (`lv_obj_create`, `lv_timer_create`, `lv_obj_del`).
Button callbacks run inside `button::update()`, which executes **outside** `display_mutex` — calling
these functions directly from a callback was a real, shipped bug: LVGL internal state slowly
corrupted over 7+ hours of runtime until UI_Task hung permanently (watchdog-unrecoverable), because
concurrent/unprotected LVGL access from two contexts is exactly the re-entrancy hazard CLAUDE.md's
"LVGL is NOT thread-safe" rule exists to prevent.

Fixed by adding `ENTER_STANDBY`/`WAKE_STANDBY` to `task_manager::UIUpdateType` and routing both button
paths through `task_manager::queueUIUpdate()` instead of calling `standby_manager::enterStandby()` /
`wakeFromStandby()` directly; `processUIUpdate()` (running on the UI Task, inside `display_mutex`)
handles them. Both button callback sites keep the direct call as a **last-resort fallback only if the
queue is full** — better a rare direct call than a dropped standby/wake request entirely.

One more piece from the same era: `wakeFromStandby()` used to call `lv_timer_handler()` directly to
force an immediate redraw. That call was removed — the UI Task already calls `lv_timer_handler()` in
its own loop, so a second, concurrent/recursive call from inside the queued handler was itself an
LVGL-corruption risk of the same shape as the bug above. The next UI Task loop iteration redraws
naturally; no explicit kick is needed. Separately, `standbyTimerCallback()` (the 3-second
overlay-to-full-standby transition) must **not** call `lv_timer_del()` on itself — it was created with
`repeat_count = 1`, so LVGL already auto-deletes it after the callback returns; an explicit delete on
top of that is a double-free that caused intermittent heap corruption on sleep entry.

The render path also has a standby guard: `flushRadarRender()` checks `standby_manager::isStandby()`
before painting, so a UI update queued just before/during the transition can't paint a screen whose
backlight is already off.

## Power Numbers

Originally measured at first implementation: **~520mA active → ~55mA standby (89% reduction)**,
**5.8h active → 54h standby** on a 3000mAh pack. **These have not been re-measured** since two
later additions that should only improve on them — CPU frequency scaling (240MHz→80MHz) and GPS's
"Aggressive 1Hz" power mode — were added after that original measurement. Treat the 55mA/89%/54h
figures as a conservative floor from an earlier, less-optimized version of standby, not a current
verified number.

## Known Issue: Standby Screen Shows Brightness, Not Battery

The overlay's "Battery: X% | Time: HH:MM" line reads `backlight::getPercent()`, not a real battery
reading — the code's own comment marks it as a placeholder (`createStandbyScreen()`,
`standby_manager.cpp`) that was never wired up. A real battery percentage is available via
`battery::getPercent()` (`include/hardware/sensors/battery.h`, used by the always-on-screen radar
battery indicator — see CLAUDE.md's Battery Monitoring System section) and would be a direct drop-in
fix if this is ever revisited.

## Statistics

`standby_manager::getStats()` returns `StandbyStats{ total_standby_count, total_standby_time_ms,
last_standby_duration_ms }`, updated in `wakeFromStandby()`. No serial command currently surfaces
these (there is no `standby` diagnostics command at all yet — everything here is button/settings-UI
driven only).

## Code References

- `include/utils/standby_manager.h` / `src/utils/standby_manager.cpp` — state machine, power
  apply/restore, inactivity timeout.
- `src/core/device_manager.cpp` — button callback (queues `ENTER_STANDBY`/`WAKE_STANDBY`,
  extra-long-press config, post-wake suppression window), `lvgl_touch_read_cb()` (touch polling
  skipped during standby, `notifyUserActivity()` on valid touch).
- `include/hardware/input/button.h` / `src/hardware/input/button.cpp` — `EXTRA_LONG_PRESS` event,
  4s/2s dual-threshold detection.
- `src/utils/task_manager.cpp` — `UIUpdateType::ENTER_STANDBY`/`WAKE_STANDBY` handling in
  `processUIUpdate()`, `checkInactivityTimeout()` call site, `flushRadarRender()`'s standby guard.
- `src/utils/settings_manager.cpp` — `KEY_AUTO_SLEEP` NVS persistence.
- `src/ui/settings_screen.cpp` — Auto Sleep dropdown (Display tab).

## Reference Documentation

- **Architecture summary**: CLAUDE.md's Standby Mode section.
- **Thread-Safety Rule** section above is a concrete instance of CLAUDE.md's general "LVGL is not
  thread-safe — only the UI Task may call LVGL functions" rule; this subsystem's 7-hour-freeze bug is
  the incident that rule exists to prevent from recurring.
