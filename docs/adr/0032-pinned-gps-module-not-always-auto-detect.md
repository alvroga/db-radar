# ADR-0032: Pinned GPS module selection (first-boot picker), not always-auto-detect

Status: Accepted
Date: 2026-08-10
Decided by: Claude (proposed, user approved)

## Context

The BH-880 (UBX binary protocol) replaced the original LC76G (NMEA/PAIR text protocol) specifically
because the LC76G has no compass — see ADR history and the "back then we never thought about the
possibility of using N-up only with GPS only" note this decision traces back to. The user wants both
modules supported again in one firmware, without risking the BH-880 path that's been field-verified
for months.

`gps_bh880.cpp` gained a second protocol parser (ported from the pre-BH-880 `gps_lc76g` driver, field-
validated 2026-02) alongside the existing UBX one, with runtime detection choosing between them
(`detectBaud()`, two strict sequential passes — UBX first, byte-identical to the original algorithm;
NMEA only attempted if that entire pass finds nothing on any of 6 baud rates). This closed the
compatibility question but left an open one: should every boot re-run that detection, or should the
module be a persisted choice?

Auto-detect-every-boot has a real cost: up to ~18s worst case if the module turns out to be the LC76G
(a full failed UBX pass across 6 rates, then the NMEA pass), and it re-derives on every boot something
that is a physical wiring fact, not something that changes boot-to-boot for a given unit.

## Decision

GPS module type is a persisted choice (`settings_manager`'s `gps_module_type`/`gps_module_configured`,
NVS-backed — confirmed to survive both the OTA-only and full-flash web-flasher images, since NVS
(`0x9000`-`0xd000`) sits in the gap between the four regions `merge_bin` writes). A one-time picker
screen appears on first boot (before `gps_module_configured` is ever true) — chosen over defaulting
silently to Auto-Detect or to BH-880, so no board ever runs on an unconfirmed guess. GPS itself still
works during that first boot via the existing two-pass auto-detect (which already ran during
`device_manager::initializeAll()`, before display/LVGL/touch are even up — the picker can't gate GPS
bring-up itself, only what *future* boots do, since it needs a working display to show). Once picked,
`device_manager::initGPS()` calls `gps_bh880::beginWithProtocol()` on every subsequent boot — skips
protocol detection entirely, runs only a single-pass baud scan restricted to the known protocol
(`detectBaudUBX()`/`detectBaudNMEA()` directly, the same two functions the auto-detect path uses,
factored out for reuse). Changing the pin (Settings > GPS) requires a reboot to apply, using the same
`esp_restart()` pattern already used by the WiFi/AP screens.

**Alternatives rejected:**
- **Always auto-detect, no pinning.** Simplest, but pays the up-to-18s worst case on every single boot
  for a fact that never changes between boots on a given physical unit. Rejected as pure waste once a
  persisted setting is this cheap to add.
- **Single interleaved detection pass** (try UBX-sync-pair and NMEA-line checks on the same byte
  stream in one loop, whichever completes first wins) — the original implementation of this feature,
  before the two-pass rewrite. Rejected specifically because a real BH-880 that also emits default
  NMEA chatter before `begin()` enables NAV-PVT could plausibly complete 2 valid NMEA lines before 3
  UBX sync pairs, misdetecting a BH-880 as NMEA and silently skipping its UBX enable command. The
  two-pass form (UBX pass 1 first, byte-identical to the pre-existing algorithm; NMEA pass 2 only if
  pass 1 exhausts every rate) makes this failure mode structurally impossible rather than merely
  unlikely — pass 1 is provably unchanged from the code that's already been field-verified.
- **Compile-time variant** (`#define` swap, matching the existing display-variant pattern in
  `system_config_variants.h`). Rejected: would require a separate firmware build per module, which
  defeats bringing LC76G support back into the *same* release binary the user flashes — the whole
  point was one firmware working on either board.
- **Default to BH-880 on first boot.** Rejected: an LC76G-only board would show no GPS fix with no
  explanation until the user happened to visit Settings — worse than a one-time picker that's shown
  exactly once, ever.

## Consequences

- **Makes easier**: instant, deterministic GPS bring-up on every boot after the first (no detection
  scan at all beyond a single-protocol baud probe); zero ambiguity between the two protocols at
  runtime, ever, once pinned; a clear, inspectable "what module is this board" state
  (`gps_bh880::protocolName()`, shown in Settings > GPS) instead of an implicit runtime guess.
- **Makes harder**: swapping the physical GPS module on an already-configured board now requires a
  trip to Settings > GPS and a reboot, rather than "just works" on the next boot. Judged acceptable —
  this is a hardware choice, not a runtime condition, and the picker/Settings path is one tap.
- **Gave up**: silently supporting an on-the-fly module swap without any user action. Not a real loss
  for this project — the two modules aren't hot-swappable without a reboot anyway (UART reconfiguration
  isn't safe to do live against the running I2C/System task's GPS read loop).
- The Settings > GPS tab's Hot/Warm/Cold Start and Factory Reset touch buttons were removed in the
  same change (unrelated to the pinning decision itself, but done together) — the web flasher gives
  full reflash control now, making those buttons redundant UI weight; the underlying commands remain
  available via the serial `gps hot|warm|cold|reset` commands for anyone who wants them.

## Addendum 2026-08-11: BN-880 as a third pinned option

**Context**: a field report (GitHub issue #1) found a board using a **BN-880** module — a
visually/name-similar but different module from the Beitian BH-880 this project targets. BN-880
speaks the same NMEA/PAIR protocol as the LC76G (confirmed on real hardware: the existing two-pass
`detectBaud()` finds it via the NMEA pass with zero code changes), but commonly carries an
**HMC5883L** magnetometer (I2C `0x1E`) rather than the BH-880's QMC5883L (`0x0D`) — a chip this
firmware had no driver for at all.

**Decision**: added `GpsModule::BN880_NMEA` as a third pinned value, reusing the NMEA protocol path
exactly (no new GPS parser). The picker and Settings > GPS dropdown both grew a third option. The one
genuinely new piece is a low-level `compass_hmc5883l` driver, dispatched to internally from
`compass_qmc5883l.cpp` — see the separate ADR on that dispatch design for why it wasn't a rename.

**No auto-detection for the compass chip, at all, ever — including on an unconfigured first boot.**
This extends the "pin the physical fact once" reasoning above one step further than GPS baud/protocol
detection already goes: `initCompass()` doesn't attempt any chip until `gps_module_configured` is
true. An earlier draft considered "try QMC5883L, fall back to HMC5883L" as a first-boot convenience —
rejected before implementation, explicitly, because the module lineup won't stay fixed at two
compass-bearing types forever, and an auto-probe can't be trusted to disambiguate a future module that
might, say, share an I2C address with an existing chip while behaving differently. Only an explicit,
physical, user-confirmed choice can. (This is the same shape of reasoning as the two-pass-not-
interleaved GPS detection above — a plausible shortcut was rejected because it can't be trusted to
stay correct as more real-world variety shows up, not because it's slow.)

**New serial escape hatch**: `gps module`, `gps module set bh880|lc76g|bn880`,
`gps module reset` (`src/utils/diagnostics.cpp`) — lets the pin be inspected, changed, or cleared
(re-showing the first-boot picker on next reboot) without touch. Doubles as a real fallback: the board
that originally reported the compass issue also had a dead touch controller, which would otherwise
make Settings > GPS physically unreachable.

**A real bug found by this field test, unrelated to the chip work itself**: picking a module in the
first-boot picker crashed (`Guru Meditation Error: LoadProhibited`) the first time it was actually
exercised on hardware — `showGpsModulePickerBlocking()` deleted the picker screen while it was still
LVGL's active screen, before the loading screen replaced it. Fixed by returning the picker object and
deleting it only after `lv_scr_load(ui.screen_loading)` in `main.cpp`. See CHANGELOG.md and the
project's existing LVGL screen-lifecycle rule (never delete the active screen before its replacement
loads) — this is that rule's second known violation, not a new class of bug.

**A second real bug, this one caused directly by the no-auto-probe decision above**: continuing the
field test onto a real BH-880 board, picking BH-880 in the first-boot picker left the compass
completely uninitialized for that entire session — Settings correctly showed BH-880 pinned, but there
was no N indicator and Heading-Up stayed unavailable, indistinguishable from an LC76G (no-compass)
board. Cause: `device_manager::initCompass()` runs during Phase 2, before the display exists and
therefore before the picker can possibly have shown yet; with `gps_module_configured` still `false` at
that point, the no-auto-probe gating (correctly, by design) skipped compass entirely for that boot —
but nothing re-ran `initCompass()` after the picker saved the pin later in the same session. The
*prior* code (pre-BN-880) never had this gap, because it always attempted the QMC5883L unconditionally
on every boot regardless of any pin — this was a regression introduced by adding the gating itself.
Fixed by having the first-boot picker reboot immediately after saving the selection
(`esp_restart()` in `showGpsModulePickerBlocking()`), the same way Settings > GPS's button and `gps
module set` already do — the pin is now guaranteed to exist before `initCompass()` ever runs again.
This means picking a module on first boot now costs one extra automatic reboot; judged an acceptable,
small, one-time cost for a compass that actually works afterward. Field-verified on the same BH-880
board: `Compass: OK`, prior hard-iron calibration reloaded from NVS unchanged.

## Addendum 2026-08-14: BE-881 as a fourth pinned option, and swapping it in for BN-880 on the picker

**Context**: a user connected a board sold as "BE-880"/"BE-881", picked the closest existing profile
(BH-880), and got a working GPS (BE-881 genuinely is BH-880-UBX-compatible) but a compass that failed
to initialize at `0x0D`. Root cause: BE-881's magnetometer is a **QMC5883P** at `0x2C` — a third
distinct compass chip, despite the naming collision with QMC5883L, confirmed against QST's official
datasheet and a live bus-scan hit.

**Decision (module addition)**: added `GpsModule::BE881_UBX` as a fourth pinned value, reusing the
BH-880 UBX protocol path exactly (no new GPS parser — same "reuse the existing protocol path"
reasoning the BN-880 addendum above used for NMEA). The one genuinely new piece is a low-level
`compass_qmc5883p` driver, dispatched to internally from `compass_qmc5883l.cpp` exactly like HMC5883L
was — see ADR-0033 for why that dispatch pattern exists rather than a per-chip rename. Compass chip
selection for BE-881 follows the same no-auto-probe rule established above: a direct, deterministic
consequence of the pin, no fallback.

**Decision (picker curation)**: the first-boot picker's three buttons changed from
BH-880/BN-880/LC76G to BH-880/BE-881/LC76G — BN-880 was swapped out, not added alongside as a fourth
button. Two real alternatives were on the table:

1. **Add a fourth button**, keeping all field-eligible-or-not modules on the picker equally.
2. **Swap BN-880 out for BE-881** (the choice made), keeping the picker at three buttons and treating
   it as a curated "field-verified" set rather than an exhaustive module list.

Rejected (1) for two reasons: first, the picker's three-button layout is already tuned tight against
the round display's safe area (see the layout comment in `main.cpp` — heights/gaps were compressed
once already, from 55px/70px to 50px/58px, to fit three buttons plus the hint label inside the visible
circular chord; a fourth button reopens that same round-vs-square margin problem class this project
has hit before, e.g. FT-09). Second, and more fundamentally: BN-880 is still only build-verified, with
an open, unconfirmed field report of unresponsive touch on this exact screen (2026-08-12) — putting an
unverified module on a picker meant to represent "known-good options for a fresh board" undersells the
risk to a new user with no other basis for choosing. BE-881, by contrast, is field-verified as of this
addendum's date. **BN-880 support is not removed anywhere** — `GpsModule::BN880_NMEA`,
`compass_hmc5883l.cpp`, the Settings > GPS dropdown, and `gps module set bn880` are all unchanged and
fully functional; only its presence on the first-boot picker changed. It can return to the picker once
it gets equivalent field verification — this is a curation call, not a deprecation.

**Field-verified 2026-08-14**: live serial capture confirmed `Compass: OK` at boot, a clean `0x2C`
bus-scan hit with zero I2C failures, and `compass read` (which goes through the real driver dispatch,
not a register poke) returning sane, changing heading data. GPS unaffected (valid fix, 5 satellites).
