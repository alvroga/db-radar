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
