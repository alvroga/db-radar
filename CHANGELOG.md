# Changelog - cc-radar Development History

All notable completed features and changes to the GPS Radar project.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Changed (docs)

**Low-priority doc trim pass — audit findings items 28-33 (2026-08-09)**

- `docs/memory_management.md` rewritten from ~180 lines of marketing prose ("enterprise-grade,"
  "expert memory engineer watching over your project," For Education/Commercial/Prototyping use-case
  sections with zero project-specific content) down to ~40 lines: verified command reference (also
  caught and removed a fabricated `memory pools test` command that doesn't exist in
  `diagnostics.cpp`), pool architecture, and the boot-loop history CLAUDE.md's own summary defers to
  this doc for.
- `docs/navigation_modes.md` trimmed ~30%: cut "Industry Examples" (aviation/marine/automotive
  comparisons, zero project-specific content) entirely, merged two sections that separately explained
  the same compass pipeline and rotation math at different detail levels into one.
- **`docs/wmm_declination.md` audit finding was checked and found incorrect** — the original 5-agent
  audit claimed it was redundant with a CLAUDE.md summary; grepped CLAUDE.md for "WMM"/"declination"
  and found zero matches, so left untouched rather than trim the only place this feature is
  documented at all down to a link that points nowhere.
- CLAUDE.md's Render Pipeline section trimmed from 130 to ~90 lines: verified the cut historical
  measurement narrative (per-stage 85ms breakdown, 240MHz scaling factors, the 2026-07-31
  beacon/sonar audit findings) is preserved in `docs/performance_optimization_backlog.md` before
  removing it here; the four numbered load-bearing constraints were left verbatim, per the file's own
  explicit rule.
- ROADMAP.md's "Waypoint Memory Optimization" entry (~50 lines, sitting under Planned despite being
  fully resolved) rewritten to ~15 lines and moved to the Resolved section where it belongs.
  "Radar Animation Effects from Capsule Radar Research" (~55 lines) had its actual research content
  moved into a new `docs/radar_animation_effects_research.md` (this was itself a documentation-
  standards violation the entry admitted to — research reported inline in conversation, never saved),
  leaving a short summary + link in ROADMAP.md.
- Added the previously-undocumented "every GPX upload/delete glitches the display" known issue to
  ROADMAP.md's Won't Fix section — it existed in CLAUDE.md's Render Pipeline section and
  ADR-0028/CHANGELOG, but a reader checking ROADMAP.md specifically (the place this project's own
  conventions say to look) would have found no mention of it at all.

### Security

**Stripped real GPS coordinates from 18 tracked compass-calibration CSVs (2026-08-09)**

`docs/calibration/*.csv` carried a `lat,lon` column pair with precise (7-decimal, sub-meter) real-world
coordinates in nearly every recorded row — clustered tightly across almost all 18 files, almost
certainly a real home/testing location. Found while fixing an unrelated hardcoded path in
`docs/calibration/analyze.py` (see below) and grepping the surrounding directory. This sat in the
**current tracked tree**, not just git history, so the fresh-squash public-release strategy in
`docs/public_release_plan.md` (which sidesteps history-only exposure) would not have protected against
it — it would have shipped directly in the public `db-radar` release. Confirmed via grep that
`analyze.py` never reads the `lat`/`lon` columns (only `course` and `speed_kn`, which don't reveal
absolute position), so the values were zeroed out in place across all 18 files — everything
`analyze.py` actually uses (magnetometer, accel, gyro, GPS course/speed, fix validity) is untouched;
re-ran the script afterward and confirmed identical analytical output.

Also fixed `docs/calibration/analyze.py`'s hardcoded absolute path
(a machine-specific `.../cc-radar/docs/calibration` path) — replaced with
`os.path.dirname(os.path.abspath(__file__))` so the script works regardless of where the repo is
cloned, instead of only on the machine and folder name it was originally written on.

**A third, more consequential instance found in the same sweep**: `ui_manager.cpp`'s radar-screen
init hardcoded the same real coordinates the CSVs carried (redacted here; the ~34.1°N, -118.1°W
area) as the pre-first-GPS-fix map
center fallback — used only until the very first NVS-saved GPS fix exists on a given device, but
that means **every fresh install of every device would have centered its very first boot on this
real location**, not a documentation example. Changed to `(0.0, 0.0)` — a neutral "no fix yet"
sentinel with identical fallback behavior (overwritten unconditionally the moment a real fix lands,
same as before). Build verified clean, no other occurrences of these coordinates anywhere in
`src/`/`include/`.

### Changed

**Renamed the build identity from `cc-radar` to `db-radar`, ahead of the public release (2026-08-09)**

`platformio.ini`'s `[env:cc-radar]` → `[env:db-radar]` (and `default_envs`), `CMakeLists.txt`'s
`project(cc-radar)` → `project(db-radar)`, the release workflow's env/build-dir/binary-filename
references (`cc-radar-esp32s3-{full,ota}.bin` → `db-radar-esp32s3-{full,ota}.bin`,
`.pio/build/cc-radar` → `.pio/build/db-radar`, the GitHub Pages URL), `web/flasher/manifest.json` and
`index.html`'s repo links, and every current/forward-looking doc citation of a `.pio/build/cc-radar/`
or `sdkconfig.cc-radar` path (CLAUDE.md, ROADMAP.md, ADR-0030, ADR-0031,
`docs/{beacon_proximity,compass_calibration_foundation,troubleshooting,firmware_installation,
waypoint_cap_increase_investigation}.md`). This repo (`cc-radar`, full history) stays untouched as
the private archive per `docs/public_release_plan.md` — this rename lands here first specifically so
the eventual fresh squash commit into the new public `db-radar` repo already has correct naming
baked in, rather than needing a second pass after the fact. Historical records were deliberately
**not** rewritten — CHANGELOG.md's own past entries, ADR-0014's `cc-radar-compass` mention (a
different, specifically-named env that existed and was removed in 2026-03), `docs/compass.md`'s same
mention, and `docs/calibration/README.md`'s description of already-captured CSV files' literal header
content all still correctly say `cc-radar`, because that's what was actually true when each was
written.

Also swapped the committed `sdkconfig.cc-radar` fallback for a freshly-generated `sdkconfig.db-radar`
(byte-identical aside from the env-name strings — diffed to confirm no drift snuck in).

**Found and fixed a real pre-existing gap while verifying the rename with a real build**: two manual
esp-nimble-cpp v1.4.1 patches (ESP-IDF 5.x compatibility — a missing `<time.h>` include and a removed
`esp_nimble_hci_and_controller_deinit()` API) had only ever been hand-applied directly inside
`.pio/libdeps/cc-radar/esp-nimble-cpp/`, which is gitignored build output, never committed, and
undocumented anywhere in the actual codebase. The env rename gave PlatformIO a fresh `libdeps`
directory and immediately exposed it: the very first `pio run -e db-radar` failed to compile. This
means **any fresh clone of this repo — including the eventual public `db-radar` release — could never
have built out of the box**, independent of the rename itself. Fixed properly by automating both
patches as a new PlatformIO pre-build extra_script, `scripts/patch_nimble_cpp_idf5.py` (same
idempotent, exact-match-or-loudly-warn pattern as the existing
`scripts/patch_i2c_master_nack_hang.py`), registered in `platformio.ini`. Verified with a true
from-scratch build (`.pio/` fully deleted, library re-downloaded) — succeeds without manual
intervention. (Noted in passing, unrelated to this fix: a from-scratch `.pio/build/` occasionally
needs one retry on the very first CMake configure pass — a pre-existing ESP-IDF/CMake quirk, not
something this rename introduced.)

**Beacon MAC configuration UI (2026-08-09)**

Settings > Beacon tab gained an "Edit" button opening a text-entry dialog (mirrors the existing AP
SSID/password dialog: on-screen keyboard, 17-char max length, Save/Cancel) to set the beacon proximity
target MAC — previously the tab only displayed the MAC read-only, and the only way to configure it was
a serial command (`beacon mac XX:XX:XX:XX:XX:XX`). Closes a real pre-release gap: beacon proximity is a
shipped, documented feature (see CLAUDE.md's Beacon Proximity System section) that was otherwise
unusable by anyone without a USB-serial connection. Field-verified on hardware. The textarea also
auto-inserts `:` every 2 hex digits as the user types (strips and rebuilds on every keystroke, so
typing or backspacing stays correct) — the user only ever types the 12 hex characters, never the
colons themselves.

**Fixed a heap-corruption crash in the auto-colon formatter above**, caught immediately on the first
hardware test (`Guru Meditation Error: LoadProhibited`, backtrace inside FreeRTOS's
`prvSelectHighestPriorityTaskSMP` — corruption surfacing far from its cause, not a straightforward
NULL-deref-at-the-call-site bug). Root cause, confirmed by reading `lv_textarea.c`: once a max length
is set, `lv_textarea_set_text()` inserts character-by-character and fires `LV_EVENT_VALUE_CHANGED`
**on every character**, not just once at the end. The reformat handler called `lv_textarea_set_text()`
again on that event, re-entering LVGL's own set-text call while its char-by-char loop was still
mid-flight, corrupting the textarea's label text buffer. Fixed with a reentrancy guard flag
(`s_beacon_mac_formatting`) so the handler no-ops for events it triggered itself.

**Fixed the auto-colon timing** — the trailing `:` was only appearing once the *next* digit was typed
(typing "00" didn't show "00:" until a 3rd character arrived), a side effect of only inserting colons
between two already-present digit groups. Now tracks the previously-displayed text
(`s_beacon_mac_prev`) to tell insertion from deletion: on insertion, a trailing colon is appended the
instant a pair completes; on deletion, it's never re-added, so backspacing "00:" collapses straight to
"00" instead of bouncing back. Field-verified on hardware (dialog open/prefill, fresh entry, and
backspace-through-colon all confirmed working).

Also added `beacon_proximity::refreshTarget()` (a thin public wrapper around the existing private
`applyTargetFromSettings()`) and call it from both the new dialog's save path and the serial `beacon
mac` command. Previously the scanning target was only re-parsed from settings on `init()` and on the
disabled→enabled edge of `setEnabled()`, so changing the MAC while already scanning at 50m zoom
silently kept matching the old address until scanning was toggled off/on — a pre-existing gap in the
serial path too, fixed for both at once rather than just papering over it in the new UI.

No format validation existed anywhere before this (the serial command only checked `strlen >= 17`);
the new dialog matches that same bar rather than inventing a stricter one, to stay consistent with the
existing serial behavior.

**Tag-triggered release pipeline + browser web flasher + 2-way Installation section (2026-08-08)**

Added `.github/workflows/release.yml`: pushing a `v*` tag builds via PlatformIO, merges
bootloader/partition-table/otadata/app into one flashable image with `esptool merge_bin`, publishes
that full-flash binary plus an app-only OTA binary to GitHub Releases, and deploys an ESP Web Tools
flasher page (`web/flasher/`) to GitHub Pages — all from the same build, so the Release asset and the
web flasher can never disagree (see ADR-0030). The web flasher's manifest lists a single merged part
at offset 0 rather than the four regions separately (see ADR-0031). README's Quick Start section
replaced with a 2-option Installation section (browser flasher / `esptool.py` + Release download) —
the PlatformIO source-build path is intentionally held back for now (repo isn't public yet, decided
same day); also fixed a stale `db-radar` clone URL and CLAUDE.md repository link that pointed at an
inactive placeholder repo instead of the real one. Full detail, including the known
`FW_VERSION`/git-tag mismatch risk, the flash-offset cross-reference to
`partitions/partitions_ota.csv`, and the repo-privacy prerequisite that's currently blocking both
paths from actually going live: [`docs/firmware_installation.md`](docs/firmware_installation.md),
[ADR-0030](docs/adr/0030-release-pipeline-build-time-pages-not-committed-binaries.md),
[ADR-0031](docs/adr/0031-single-part-manifest-not-multi-part.md). **Not yet exercised end-to-end on a
real tag push or real hardware** — see that doc's Verification status section.

### Changed

**Trimmed `docs/performance_optimization_backlog.md` from 2,116 to ~1,140 lines (2026-08-07)**

Condensed the repeated stage-by-stage `perf` HUD capture tables (superseded by CLAUDE.md's Render
Pipeline section and the C1–C7 completed-work entries) and removed the original 2026-07-27
pre-measurement analysis (Tiers 0–3, the "measure first" section) outright — it predated every real
measurement in the document and two of its confident conclusions were wrong, in ways already
corrected inline elsewhere. Preserved in full in git history as of this date; the architectural
decisions it led to are recorded properly in ADR-0004, ADR-0005, ADR-0007, ADR-0008. Kept intact:
the live work queue (with its stale "I2C freeze root cause unsupported" bullet corrected to point at
the actual fix, ADR-0021), the "residual trap" methodology section (referenced from CLAUDE.md), §7/§8
(beacon and sonar/buzzer audits — ROADMAP.md's "Full analysis" link targets), and the all-steps status
table. `ADR-0004`'s citations into the removed section were also decoupled from exact line numbers so
the removal doesn't strand them. No code changes.

**Hoisted per-waypoint `cos`/`sin` out of the radar render loop (2026-08-07) — backlog §3.6**

`rotatePoint()` recomputed `cos()`/`sin()` (double-precision) for every waypoint, every frame, even
though every waypoint in a frame rotates by the same `ui.current_heading`. Split into
`rotatePointFast(cos_a, sin_a, ...)` and hoisted the single `cosf`/`sinf` call to once per frame in
`drawWaypoints()`; `rotatePoint()` survives as a thin wrapper for the single-point call sites
(`latLonToScreen()`, tap hit-testing) where there's nothing to hoist. Pure de-duplication, no behavior
change — `pio run` clean, RAM/flash unchanged (51.2% / 40.7%). See
[`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md) §3.6.
`getColorScheme()` caching, filed alongside this in the same backlog item, turned out to be a false
lead — it already gates its only I/O behind a 1s `millis()` check, so there was nothing to fix.

**GPX web manager: storage gauge and file count now refresh after upload/delete, not just on page
reload (2026-08-07) — field-verified**

The "Flash storage (GPX)" gauge and "GPX files" count are already live reads (`esp_vfs_fat_info("/ffat")`
and a directory listing, both queried fresh per request — no reboot or device-side caching was ever
involved). The bug was purely client-side: `loadStorageInfo()` only ran once, on initial page load —
`triggerReload()`'s three call sites (batch upload, batch delete, single delete) all refreshed the file
list (`loadFileList()`) but never re-fetched `/storage`, so the gauge stayed frozen at whatever it read
when the page opened until the user manually reloaded the browser tab. Added a `loadStorageInfo()` call
alongside each of those three `loadFileList()` calls. Also removed the now-stale "Reload the page to
refresh the count" line from the page's own Auto-load info box, and added a small animated
`.`/`..`/`...` ellipsis (`data-loading` attribute + a 400ms interval) to the "loading" placeholder text
on `storageText`/`fileCountText` so the initial fetch doesn't read as stalled.

`pio run` clean. Flash +1,072 B (embedded HTML/JS string), RAM unchanged. **Field-verified 2026-08-07**:
confirmed working on hardware.

**Fixed-waypoint live distance now works for off-screen waypoints; added a "locked on" icon;
off-screen indicator range raised 10×→100× (2026-08-07) — field-verified**

Follow-up to the "Off-screen waypoint indicators: tappable" change directly below. That change added
a one-shot DISTANCE row to the waypoint detail screen, but the actually-requested behavior — the same
continuously-updating "Fixed: Xm" readout the middle-left HUD already shows for on-screen fixed
waypoints — silently didn't work for anything off-screen: `updateRadarDisplay()`'s fixed-waypoint
label auto-unfixed at a hardcoded 1km regardless of how far the fixed waypoint actually was, so fixing
a farther/off-screen waypoint would show "Fixed:" for one frame and then immediately auto-release.

- The hardcoded 1km auto-unfix is now `RadarConfig::FIXED_WAYPOINT_MAX_DISTANCE_M` (100km, raised from
  an initial 20km same-day pass — 20km was itself still blocking legitimate fixes on distant off-screen
  waypoints; 100km now matches `DISTANCE_FILTER_MULTIPLIER`'s own cutoff at 1km zoom) — a safety net
  for a stale fix, not a normal-use limit — and the label formats km above 1000m (`"Fixed: %.1f km"`),
  matching the detail screen's DISTANCE row convention.
- Added `fixed_waypoint_icon`: a 45×45 `lv_canvas` "locked on" icon (filled dot + ring), monochrome
  white/black matching the rest of the HUD (not the waypoint's own yellow/dark-orange — this is chrome,
  not a map object), positioned above the distance label, tappable as a second target for the same
  action (open the fixed waypoint's detail screen). Drawn by the new
  `navigation::drawFixedWaypointIndicator()`, following the existing `drawBeaconFoundIndicator()`
  pattern (same canvas-buffer/hide-show/theme-redraw lifecycle). Visibility is driven from the same
  per-frame block as the label, not independently. Sized 45×45 (50% larger than the initial 30×30 pass)
  per field feedback on legibility.
- `RadarConfig::DISTANCE_FILTER_MULTIPLIER` raised 10.0→100.0: sector clustering already caps visible
  off-screen triangles at `MAX_OFFSCREEN_INDICATORS` (8) regardless of how many candidates pass this
  filter, so a wider cutoff only changes which waypoint fills a sector when nothing closer exists in
  it — not how much clutter is shown. It's also meaningful, not just harmless: the two-tier index's
  working-set selection (`gpx_loader::selectAndMaterialize()`/`reselect()`, ADR-0023) picks the N
  globally-closest waypoints with no distance cap, so there are real far-away candidates for a wider
  cutoff to surface. `docs/waypoint_filtering.md` updated throughout (multiplier, worked examples,
  "Important Constants" block) — and its pre-existing stale `MAX_WAYPOINTS = 500`/`INDICATOR_SIZE = 15`
  figures were corrected to the actual current values (200, 25) while touching those same blocks; the
  `500` was never re-synced after that cap was rolled back to 200 the same day it shipped (ADR-0022's
  own addendum) and then superseded by the two-tier PSRAM index (ADR-0023).

`pio run` clean. RAM +6,072 B (dominated by the 45×45 `TRUE_COLOR_ALPHA` canvas buffer, ~6,075 B at
`LV_COLOR_DEPTH=16`) / Flash +724 B. **Field-verified 2026-08-07**: confirmed working on hardware,
including fixing an off-screen waypoint beyond the old 20km cap.

**Off-screen waypoint indicators: tappable, and distance now shown on the detail screen
(2026-08-07) — build-verified, not yet field-tested**

Two related gaps in `docs/waypoint_filtering.md`'s own "Future Enhancements" list (#3 "Distance
labels", #6 "Touch interaction") — the orange off-screen triangles were pure decoration: no tap
handler, and no distance shown anywhere, even for on-screen waypoints, once opened. Fixed both:

- `drawWaypoints()` (`navigation.cpp`) now persists each drawn indicator's screen position,
  source waypoint index, and distance into a new file-static `g_offscreen_tap[]` array (8 sector
  slots + 1 for the fixed waypoint) every frame. `handleTapAt()` hit-tests against it (24px radius)
  after the existing on-screen-dot loop finds no hit, and opens the same waypoint detail screen a
  tapped on-screen dot would. No mutex needed — both the draw callback and the touch callback run
  on the UI Task, unlike `ui.waypoints[]`/`selected_waypoint_index`, which the System Task's
  reselect also touches (existing mutex use there is unchanged). A tap target whose waypoint slot
  got recycled between draw and touch is dropped defensively, same pattern the on-screen path
  already used.
- `waypoint_screen.cpp` now shows a DISTANCE row (Haversine via the existing `utils/geo.h` helper,
  not navigation.cpp's radar-scale equirectangular approximation — this is a one-shot compute on
  screen open, and an off-screen-tapped waypoint can be well outside that approximation's accurate
  range) formatted as `"820 m"` under 1km, `"%.1f km"` at or above it — so a 5/10/15km+ waypoint
  reads as a real distance instead of not being shown at all. Hidden if there's no GPS fix yet
  (`center_lat`/`center_lon` both 0), matching the existing guard pattern used elsewhere in this file.

At 1km zoom the existing `DISTANCE_FILTER_MULTIPLIER = 10.0` means off-screen indicators can appear
for waypoints up to 10km away (zoom_radius × 10, unchanged by this fix, see
`docs/waypoint_filtering.md`) — the distance now shown on tap makes that reachable-or-not distinction
visible at a glance instead of requiring a guess. `pio run` clean, RAM +176B / Flash +1,084B (new
static array + UI code, no PSRAM/SRAM budget concern). Not yet tested on hardware.

**Boot loading screen: spinner no longer freezes for the whole GPX index scan (2026-08-07) —
build-verified, not yet field-tested**

Field-reported after the `MAX_INDEX_FILES` 512→1024 test below: boot took visibly longer, and the
loading spinner appeared stuck (not animating) for the last few seconds despite serial output still
advancing — the device wasn't actually hung, but had no way to show that. Root cause:
`ui_manager::updateLoadingStatus()` deliberately never calls `lv_task_handler()` (see its own NOTE —
doing so *after* `task_manager::startTasks()` is the exact bug `standby_manager.cpp` hit once before,
UI Task freeze from a second concurrent LVGL caller), and `main.cpp`'s boot sequence itself only calls
`lv_task_handler()` once, right after the loading screen is first shown. Every phase between then and
`startTasks()` — including the single largest one, the GPX folder scan — ran with LVGL never given
another chance to paint a frame, so the spinner animation and status text both sat frozen even though
real work was progressing underneath.

Fixed narrowly rather than touching the shared function's guarded behavior: added a local
`setLoadingStatus()` wrapper in `main.cpp` (pumps `lv_task_handler()` right after each status
update) used for every boot-sequence call site, all of which run before `startTasks()` — the same
single-threaded-LVGL window `main.cpp` already relied on for its one existing `lv_task_handler()`
call. For the GPX scan specifically — the one phase with no intermediate status update at all —
`gpx_loader::loadAllGPXFiles()` gained an opt-in `show_boot_progress` parameter (default `false`,
explicitly documented in `gpx_loader.h` as boot-only) that makes `loadAllGPXFilesIndexed()` pump a
live `"Loading waypoints... (N)"` status + `lv_task_handler()` every ~150ms during the per-file scan
loop. Only `main.cpp`'s boot call passes `true`; `refreshGPXFiles()` (the post-boot `/reload` HTTP
path, running while the UI Task owns LVGL) is untouched and still defaults to `false` — passing `true`
there would reintroduce exactly the concurrent-caller bug this avoided. `pio run` clean, RAM/Flash
usage unchanged from the pre-fix build (logic-only change). Not yet tested on hardware — the actual
animation smoothness and whether 150ms is a good throttle interval need a real boot to confirm.

**`MAX_INDEX_FILES` raised 512 → 1024 — field-verified working, kept (2026-08-07)**

Follow-up to the 512 raise below: 512 loaded fast on hardware in the prior field test, so raised to
1024 as one more stress-test data point rather than a documented design requirement (the current
real-world file count is nowhere near either number; `docs/quests_plan.md`'s badge worst-case math is
computed against ≤512, still valid as a subset). PSRAM-only cost — `IndexFile` is 96B/slot, so the
change is +49,152B PSRAM, no static SRAM/Flash impact, confirmed via `pio run` (RAM 49.3%/Flash 40.6%,
unchanged). **Field-verified**: boots successfully at 1024, no reported functional regression — boot
did take visibly longer, which turned out to be the loading-spinner-freeze bug above (now fixed)
making an already-longer scan look stuck, not a new performance problem at 1024 itself. Kept at 1024.
Also generated a 512-file test batch with realistic future badge payloads (bogus `<quest:badge>` hex,
640/1,088 chars matching the 24×24/32×32 tiers in `docs/quests_plan.md` §7) to see real file-size
growth: 893B (24×24) / 1,341B (32×32) per file, ~558KB for all 512 — trivial against the ~7.69MB FFat
budget. That exercise also surfaced a real future landmine, documented in `docs/quests_plan.md` §7's
open-items list rather than fixed now (nothing parses `<quest:badge>` yet): `buildFileIndex()`'s
512-byte line buffer (`gpx_loader.cpp`) will split both badge hex tags across 2-3 reads, so whoever
implements badge extraction can't reuse the existing single-line `strstr()` pattern as-is.

**GPX batch upload/delete: O(N²) reload cost fixed; flash-suspend enabled against display
corruption during heavy FFat writes (2026-08-07) — field-verified**

Two findings from stress-testing `MAX_INDEX_FILES = 512` with a real 512-file upload/delete batch,
done in chunks of 100: (1) timing was clearly non-linear — first 100 files took 1m22s, the next 100
took 3m36s (>2.6x for an equal-sized batch) — and (2) the display showed visible "interference"
(shifted/wrapped frame content) on every single upload and delete.

**(1) Fixed the O(N²) reload.** `upload_handler()`/`delete_handler()` (`gpx_server.cpp`) each
triggered a full `gpx_loader::refreshGPXFiles()` — reset the whole PSRAM index and rescan every file
present — after every single request, so a batch of N files did N full rescans of an ever-growing
folder. Removed the auto-reload from both handlers entirely; added `POST /reload` that does what the
removed code did, called once by the client (`handleFiles()`/`deleteSelected()`/`deleteFile()`)
after a batch finishes rather than after every file. Also moved the client's `/list` refresh (which
itself reads every file's cached name) out of the per-file loop to run once. Converts total batch
cost from O(N²) to O(N). Boot was never affected by this — it only ever calls `loadAllGPXFiles()`
once. Full reasoning, including two rejected alternatives:
[ADR-0028](docs/adr/0028-defer-gpx-reload-to-explicit-endpoint.md).

**(2) Enabled `CONFIG_SPI_FLASH_AUTO_SUSPEND`** (`sdkconfig.defaults`). Every GPX upload/delete does
a real FFat (internal SPI-NOR flash) write or erase; ESP-IDF's flash driver briefly disables both
cores' cache/interrupts during that operation, which can starve the RGB LCD panel's continuous DMA
refill — a plausible, structurally-consistent explanation for the observed corruption, though not
confirmed by direct instrumentation. `CONFIG_SPI_FLASH_YIELD_DURING_ERASE` was already on (erase
already chunks itself) but couldn't preempt mid-chunk; `AUTO_SUSPEND` (ESP32-S3 hardware feature)
can pause a flash op for higher-priority work instead. `sdkconfig.cc-radar` regenerated and diffed —
only the intended flag and its automatic Kconfig-dependency siblings changed, nothing else drifted.

**Field-verified 2026-08-07**, completing the remaining ~312 files of the original 512-file stress
test: an equal-sized 100-file batch that took 3m36s before this fix took 24s after — ~9x faster,
confirming the O(N²)→O(N) reload fix. All 512 files uploaded successfully, and radar boot/render
time was unaffected by the much larger file/waypoint count. The flash-suspend change does not
appear to have resolved the display corruption, though its effect wasn't isolated from the reload
fix in this test — the corruption is now treated as accepted, expected behavior (see the web UI's
new "Note" box below) rather than pursued further. Full verification detail:
[ADR-0028](docs/adr/0028-defer-gpx-reload-to-explicit-endpoint.md)'s Verification status section.

**GPX web manager: upload/delete progress feedback, file-count display, and a display-glitch note
(2026-08-07)**

Follow-up polish after the 512-file stress test above, driven by two real gaps it exposed: (1) the
only feedback during a long batch was watching the physical device's screen glitch on each write —
not exactly reassuring — and (2) the page's "Waypoints loaded: X / 200" indicator was nearly
useless as upload feedback, since it reflects the live GPS-position working set (capped at 200) and
saturates almost immediately regardless of how many of the 512 files actually got indexed
correctly.

Added a persistent progress indicator (`gpx_server.cpp`, separate `<div>` from the existing
success/error status box so it doesn't fight that box's 5-second auto-hide) showing `Uploading 45 /
312: FILE.GPX` (or `Deleting …`) live per-file during `handleFiles()`/`deleteSelected()`, switching
to `Rebuilding index` during the batch's single `/reload` call, with a pure-CSS animated ellipsis
(no extra JS timers). Replaced the "Waypoints loaded" indicator with a "GPX files: N / 512" count
under the storage-usage bar — `storage_handler()` now also returns `file_max` (from
`gpx_index::MAX_INDEX_FILES`) so the page doesn't hardcode a second copy of that constant; current
count comes from `/list`'s response length, already fetched. Added a second info-box, matching the
existing "Auto-load" one's style, noting that upload/delete will visibly glitch the display — this
is expected, not a malfunction, per the finding above.

**SD→FFat GPX auto-migration removed — was silently restoring deleted files (2026-08-07)**

Field-testing quest deletion (delete-all on 18 GPX files via the web manager) surfaced a real bug:
files reappeared, unchanged, on the very next reboot, and again on a second reboot. Root cause:
`gpx_loader::migrateFromSDIfNeeded()` (shipped same-day as ADR-0024) decided whether to copy files
from the old SD location into FFat based on "is the FFat gpx folder currently empty" — a heuristic
that can't tell "never migrated" apart from "user just deleted everything on purpose." The original
migration copied from SD, never deleting the SD source, so an intentional delete-all made FFat empty
again and the next boot silently re-copied every "deleted" file straight back. Fixed in two passes:
first replaced with an NVS-flag-gated one-time purge that also deleted the stale SD copies (field-
verified — deleted files stayed deleted across reboots), then removed entirely at your request, since
dormant automatic-delete logic has no reason to keep living in the firmware once it's done its job.
FFat is now the sole, permanent source of truth for GPX files — no migration path exists in either
direction. Files: `src/gpx/gpx_loader.cpp`, `CLAUDE.md`, `ROADMAP.md`.
Full reasoning: [ADR-0027](docs/adr/0027-remove-sd-ffat-gpx-migration.md).

**`gpx_index` file capacity raised for quests: `file_id` widened to `uint16_t`, `MAX_INDEX_FILES` 64
→ 512 (2026-08-07)**

Prep work ahead of the Quests feature (see `docs/quests_plan.md`), which will make many more,
smaller GPX files realistic than the current handful of large ones. `gpx_loader.cpp`'s two
`open_file_id = 0xFF` sentinels made any `uint8_t`-typed `file_id` cap unsafe past 254 (255 collides
with the sentinel). Rather than just raising the byte-frugal cap again, re-examined the type itself:
a compiled struct-layout test confirmed widening `file_id` to `uint16_t` costs **zero** extra PSRAM
per `IndexEntry` (absorbed by existing 4-byte struct alignment padding, since `float`/`uint32_t`
members already force the struct to that boundary), while `uint32_t` would have cost +32KB for no
benefit. Sentinels widened to `uint16_t open_file_id = 0xFFFF` to match, `MAX_INDEX_FILES` raised to
512 (a `static_assert` now pins it below `0xFFFF`). Also added `gpx index genfiles <lat> <lon>
[count]` / `genfiles clean` debug commands (`diagnostics.cpp`) — the existing `gentest` command only
ever stress-tested `MAX_INDEX_ENTRIES` (many waypoints in one file); there was no existing way to
stress-test `MAX_INDEX_FILES` (many separate files) at all. `pio run` clean; RAM/Flash static usage
unaffected (the file table is PSRAM-only). Hardware stress test at 512 files not yet run.
Full reasoning: `docs/quests_plan.md` §0.5.

**CRT scanline render effect built, measured on hardware, and reverted — rejected on brightness, not
performance (2026-08-06)**

Follow-up on ROADMAP.md's "CRT / 8-bit Display Theme" backlog item: the per-pixel scanline route it had
provisionally passed over (2026-08-04, in favor of a font-only approach) was actually built and
measured rather than left as an estimate, since this render path has a history of wrong estimates (see
CLAUDE.md's Render Pipeline section). Implemented as a halve-brightness darken on alternate output rows
inside `rotate90_tiled`'s existing scatter pass in `device_manager.cpp`, behind a runtime-only
`rot scanline on|off` serial toggle (no NVS persistence, no settings UI). Measured on real hardware:
`tiled rotate` 38.9ms → 42.1ms, frame 80.2ms → 86.2ms (+6ms, ~7.5%) — a real cost, not free as
hoped, because darkened rows give up the pass's bulk `memcpy` for a scalar per-pixel store loop. That
cost turned out not to be the deciding factor: on-device visual check showed a ~20% perceived brightness
loss on a display already run near the floor of outdoor readability, which was rejected outright.
**Fully reverted** — `device_manager.cpp`, `device_manager.h`, `diagnostics.cpp` are byte-identical to
before the experiment (verified via `git diff`), nothing shipped. The font-only fallback scoped
2026-08-04 was reconsidered afterward and **won't be pursued either** — the investigation was about the
CRT/8-bit scanline *look*, not the font in isolation, and a font swap alone doesn't deliver that look.
Whole backlog item closed. Full writeup:
[ADR-0026](docs/adr/0026-crt-scanline-brightness-rejected.md) · ROADMAP.md → "Won't Fix" →
"CRT / 8-bit Display Theme".

**Version scheme changed from vYY.MM.DD to vYY.MM.## (2026-08-06)**

`FW_VERSION` was one build per calendar day, which undercounts on days with multiple builds and
stays static across a whole day of iteration. `scripts/gen_version.py` now writes `vYY.MM.##`, a
counter that increments on every build and resets to `01` when the year/month rolls over. There's
no separate state file — the counter is recovered by parsing the previously committed
`fw_version_gen.h` (the same "committed fallback" file the script already overwrote every build);
if that file is missing or its version string doesn't parse, the counter restarts at `01`.
`FW_BUILD_TS` (the per-build Unix timestamp behind `FW_STAMP_VAL`'s reflash-detection) is unchanged.
Files: `scripts/gen_version.py`, `include/core/fw_version_gen.h`, `README.md`.
Full reasoning and the state-file alternative considered: [ADR-0025](docs/adr/0025-version-scheme-monthly-build-counter.md).

**GPX/logs web page layout fixes: spacing and left-justified names (2026-08-06)**

Follow-up polish on the GPX-to-FFat web UI below, from a screenshot review after field-testing. Three
CSS-only fixes in `gpx_server.cpp`: the GPX page's `.bulk-actions` row (Select all / Download Selected
/ Delete Selected) sat flush against the drag-and-drop zone with no gap (`margin-top: 20px` added);
the GPX file list's name/code text was visually centered in each row rather than left-justified —
`.file-item` is a 3-child flexbox with `justify-content: space-between` and the label had no
`flex-grow`, so it centered in the middle gap between the checkbox and the buttons (`.file-label` now
`flex-grow: 1`, matching the pattern the logs page's `.log-info` already used correctly); and the logs
page's checkbox sat glued against the filename with no gap — `.log-checkbox` was missing the
`margin-right: 10px` that the GPX page's `.file-checkbox` already had.

**OTA partitions grown 2MB → 4MB per slot, reclaimed from unused FFat (2026-08-06)**

`partitions_ota.csv` had been unchanged since project inception — a canned Arduino IDE preset never
revisited for this project's actual needs. Investigation found two things: OTA slots were at 80% used
(409KB headroom) against a project history of consistent flash growth, and the 11.7MB FFat partition
was entirely unmounted — a full grep for FAT-on-flash mount calls found only `/sdcard` (the SD card),
never `ffat`. Both slots grown to 4MB (now 40.0%/2.4MB headroom), leaving FFat at ~7.69MB — still
oversized for GPX storage at the project's real data density (see ADR-0024's capacity math: ~169
bytes/waypoint for the project's own lean generator format vs. ~5-11KB/waypoint for Geocaching.com
imports, byte-counted from real files in `assets/gpx/`). Landed on 4MB, not the 3.5MB first considered
same day, because OTA headroom can only be replenished with a full USB reflash while FFat headroom can
be freed anytime over the web portal — that asymmetry makes OTA the better place to spend "free"
headroom once both are already oversupplied for their actual use (see ADR-0024's addendum). Also
decided as part of this change: GPX storage will move from SD to FFat — the SD card is physically
inaccessible without disassembling the device in the current enclosure, a bad place to keep a hard
dependency for core functionality. SD keeps dev-only logging for now and stays in the design for a
specific deferred future use (offline map-tile caching, no current priority). The migration itself
shipped the same day — see the entry below.
Full reasoning and alternatives considered: [ADR-0024](docs/adr/0024-ota-partitions-grown-from-unused-ffat.md).

Build verified against the new table: 40.0% flash (1,678,507 / 4,194,304 bytes), unchanged RAM.

**GPX storage moved from SD to FFat; web logs page gated behind dev mode (2026-08-06)**

Follow-through on the decision above. `device_manager::initFFat()` mounts the `ffat` partition
(`esp_vfs_fat_spiflash_mount_rw_wl()`, wear-levelled, `format_if_mount_failed=true` for the
first-ever mount after a repartition) at `/ffat`, right after SD init in `initializeAll()` — SD stays
mounted too, since dev-only logging keeps living there per ADR-0024. `gpx_loader.cpp` and
`gpx_server.cpp` both now read/write `/ffat/gpx` instead of `/sdcard/gpx`. `gpx_loader::init()` runs a
one-time migration on boot: if the new FFat folder has no `.gpx` files yet but the old SD folder does,
it copies them across (byte-for-byte, `fopen`/`fread`/`fwrite`) so upgrading firmware doesn't orphan
already-uploaded waypoints on a now-unread SD path — it only ever runs once in practice, since after
the first copy the FFat folder is never empty again.

The web GPX manager (`/`) gained a storage gauge: a new `/storage` endpoint calls
`esp_vfs_fat_info("/ffat", ...)` and returns `{total, free, used, percent}`; the page renders it as a
color-coded bar (green <70% used, amber <90%, red beyond) next to the existing waypoint count. The
gauge and the "Auto-load" info box were placed right under the nav buttons, above the drop area — the
first thing a user sees is capacity, not the upload control.

The GPX page also gained the bulk select/delete/download UI the logs page already had: a "Select all"
checkbox plus "Download Selected"/"Delete Selected" buttons above the file list, with a checkbox on
each file row. The logs page in turn gained "Download Selected" next to its existing "Delete Selected"
— neither page had both actions together before this pass. There's no on-device zip capability, so a
multi-file download just fires one `<a download>` click per selected file, staggered ~400ms apart to
avoid the browser's multi-download prompt; each file still saves individually, same as a single-file
download does.

The `/logs` page and its supporting endpoints (`/logs-list`, `/delete/logs/*`, `/download/logs/*`) now
check `settings_manager::getSettings().dev_mode` server-side and return 404 when it's off — logging
stays SD-only and dev-mode-only per ADR-0024, so a normal user should never be able to reach a page
about it. The upload page's "System Logs" nav link is hidden client-side too (`/dev-status` endpoint,
JS on page load) so there's no dead link when dev mode is off, though the server-side 404 is the real
gate. The logs page's own info box was reworded to state plainly that logs live on the physical SD
card while GPX waypoint files now live on internal flash — the two storage locations diverge for the
first time with this change, so the page needed to say so.

Build verified: Flash 40.5% (1,698,451 / 4,194,304 bytes, +19,944 B over the ADR-0024 baseline
including the bulk select/download UI above), RAM 49.3% (161,584 / 327,680 bytes, +80 B).
**Field-verified 2026-08-06**: first-boot FAT formatting and the SD→FFat migration copy both worked —
waypoints uploaded before the migration are present and load correctly from `/ffat/gpx` after
reflashing. The dev-mode-gated logs page and the bulk select/download UI have not specifically been
exercised on hardware yet.

### Fixed

**Waypoint on-screen test used a square bounding box on a round display (2026-08-06)**

Reported by the user with two annotated screenshots: a waypoint could visibly vanish when zooming in
(specifically noticeable 100m → 50m), reappearing only once the user got close enough or zoomed back
out. `drawWaypoints()`'s on/off-screen decision (`src/ui/navigation.cpp`) was `x >= 0 && x <
screen_size && y >= 0 && y < screen_size` — a test against the full 480×480 square framebuffer. The
physical glass is round: a waypoint whose position falls in the square's corners (outside the round
visible radius, but still inside the 480×480 bounds) passed this test and was drawn as a dot in the
area hidden under the round bezel, instead of falling through to the existing off-screen sector-arrow
logic. A follow-up screenshot confirmed it directly — the "vanished" waypoint's dot was sitting in the
black square corner outside the visible circle, not merely off the edge.

Fixed by replacing the square test with a circular one (`dx²+dy² <= (screen_size/2)²`, relative to
screen center) in both `drawWaypoints()` and the tap hit-test in `handleTapAt()` — the latter needed
the same fix for consistency, since a waypoint drawn as an off-screen arrow could otherwise still
register a tap in the hidden square corner where its (undrawn) dot would have been.

This is distinct from FT-03 (zoom radii not a geometric progression) and the milder, purely
geometric quadrant-vs-inscribed-circle effect the user's first two screenshots also raised — a
waypoint that's genuinely just past the new zoom's true visible radius after zooming in. That one
remains: the fix here only stops waypoints from being *incorrectly* drawn on-screen when they're
actually off-screen; it doesn't change when a waypoint legitimately falls outside a smaller zoom
radius. Build verified: RAM 49.3% (161,584 B), Flash 40.5% (1,698,859 B) — unchanged.
**Field-verified 2026-08-06**: user confirmed on hardware the 100m → 50m corner case now behaves
correctly. Files: `src/ui/navigation.cpp`. See ROADMAP.md → Resolved → "FT-09".

**FT-03 and FT-05 closed as stale ROADMAP entries, not code fixes (2026-08-06)**

Both entries described symptoms that no longer reproduce, and in both cases the likely explanation is
the same: the zoom levels and the waypoint render path were each revised at some point without
ROADMAP.md being updated to match, not that either issue was silently fixed by design. FT-03 was
written against a since-replaced zoom set (50m/100m/500m/1km/5km, 2×/5× jumps); the current set
(`RadarConfig::ZOOM_CONFIGS[]`: 50/100/200/500/1000m) is mostly 2× steps with one 2.5× jump and was
confirmed to feel natural on hardware, no code change made. FT-05 (dot and off-screen arrow rendering
simultaneously) could not be reproduced despite deliberately retrying the original walk-into-bounds
scenario during the FT-09 investigation above — `drawWaypoints()`'s current `if (on_screen) {...}
else {...}` structure makes the two mutually exclusive by construction, and the default full-frame
redraw (`full_refresh = 1`) leaves no stale pixels to linger; most likely resolved incidentally by the
zero-copy render rewrite, but the exact fixing commit wasn't pinned down. See ROADMAP.md → Resolved →
"FT-03", "FT-05".

**FT-08: `field_log` writer task/PSRAM ring kept running after dev mode was turned off (2026-08-06)**

Found while investigating storage architecture for ADR-0024. `field_log` (the PSRAM-ring field-data
logger for hardware bring-up/tuning) allocates a ring buffer and a dedicated writer task in `begin()`,
called once at boot when `dev_mode` is on (`main.cpp:99`) — but there was no `end()`/`stop()`
counterpart anywhere in the module, only per-session `startSample()`/`stopSample()`. Turning dev mode
off mid-session via the serial `dev off` command correctly stopped `system_logger` but left the
field_log writer task and ring buffer running for the rest of the session regardless.

Added `field_log::end()`: stops any sample in progress via the existing stop path, signals the writer
task to exit at a safe point (state `IDLE`, no open file) and waits briefly for it to self-delete, then
frees the PSRAM ring and control mutex and clears the "begun" flag so a later `begin()` can restart
cleanly. Wired into `handleDevCommand`'s `"off"` branch (`diagnostics.cpp`) alongside the existing
`system_logger` disable call. Dev mode has no other toggle path in the codebase (no separate UI code
calls `saveDevMode` directly), so this closes the leak completely rather than only for the serial path.

Build verified: RAM 49.3% (161,584 B), Flash 40.5% — unchanged from the ADR-0024 GPX/FFat baseline.
**Field-verified 2026-08-06**: `dev off` printed `[FLOG] Stopped, writer task and PSRAM ring freed`
before the `[DEV] Dev mode OFF` line, and a follow-up `flog start flat360` returned `[FLOG] begin() not
called` — confirming the teardown actually ran (task deleted, ring freed) rather than just logging a
message. Files: `src/utils/field_log.cpp`, `include/utils/field_log.h`, `src/utils/diagnostics.cpp`.
See ROADMAP.md → Resolved → "FT-08".

### Added

**Two-tier waypoint index — closest-N selection instead of a higher SRAM cap (2026-08-05)**

`gpx_loader::loadAllGPXFiles()` previously filled the 200-slot `ui.waypoints[]` array in filesystem
order — a user with waypoints across many files could get 200 that happened to load first, possibly on
the other side of the world from their actual position, with no way to fix it short of raising
`MAX_WAYPOINTS` again (the exact mistake ADR-0022 already made and rolled back the same day). Instead:
every waypoint across every GPX file is now indexed in PSRAM (`gpx_index`, new module, up to 8192
entries, `{lat, lon, file_offset, file_id, found}` each), and the 200-slot SRAM array is a working set
of the closest 200 to the user's position, computed via Haversine + `std::partial_sort`
(`gpx_loader::selectAndMaterialize()`) and kept current via a movement-triggered delta reselect (System
Task, >150m moved, only when the index exceeds the working set) that re-parses just the handful of
slots that actually changed rather than rescanning every file. `fixed_waypoint_index` survives a
reselect if its slot's source entry is unchanged; `Waypoint::found` writes through to the PSRAM index
so it survives a slot being recycled. Also closes a pre-existing gap where deleting a GPX file
(`gpx_server.cpp`'s `delete_handler`) never triggered a reload at all. New `gpx index` serial command
reports index/working-set size and truncation state. `MAX_WAYPOINTS` itself is untouched (still 200,
the boot-verified ADR-0022 floor) — measured static SRAM cost of the whole feature is +3,640 B
(+1.1 percentage points).

**Field-verified on hardware** with a 16-file/515-waypoint SD card, cross-checked against an
independent Haversine oracle script: cold selection, a forced high-churn reselect (174/200 slots,
457ms, real SD `fseek`+re-parse, Sydney-area synthetic center), HDOP gating, the 150m movement
threshold's correct silence while stationary, and a live end-to-end test writing 50 fresh waypoints
near the real GPS position and confirming they correctly took over the top of the working set — all
matched independently-computed ground truth exactly. This verification pass also caught and fixed a
real bug: `gpx_loader::init()` (which allocates the PSRAM index) was never called anywhere, so the
feature was silently dead on any board until now — see `main.cpp`. Still open: the automatic GPS-driven
reselect call site wasn't observed firing from real (non-injected) movement, and concurrent-SD-access
safety during a reselect is unverified. See [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md) and
`docs/waypoint_two_tier_index_plan.md` for the full verification detail.

### Fixed

**Boot failure at `MAX_WAYPOINTS=500` — FreeRTOS task creation failed on real hardware (2026-08-05)**

The 500-waypoint cap shipped earlier the same day (see below, and ADR-0022) passed its static-SRAM
budget check but never booted: `xTaskCreatePinnedToCore` for the Network and System tasks returned
`pdFAIL`. Root cause: static SRAM growth (133,056 B → 197,856 B, 40.6% → 60.4%) ate directly into
internal heap available at runtime — `[BEACON] Free internal SRAM before BLE init: 83899 bytes` left
too little once BLE (~25KB) and the UI/I2C task stacks (24KB) were granted. `MAX_WAYPOINTS` is now
**200** (`include/ui/ui_manager.h`), restoring ~127KB free before BLE init — **field-confirmed booting
on hardware, BLE active.** The equirectangular render-cost fix from the same change is unaffected and
stays. 200 is a conservative floor (a row already computed in ADR-0022's table), not a measured
ceiling — see the ADR-0022 addendum and ROADMAP.md's Waypoint Memory Optimization entry for what a
higher cap would need.

**FT-06 / FT-07 I2C bus freeze — field-verified resolved (2026-08-05)**

The recurring full/partial interface freeze (button, touch, display unresponsive) is confirmed fixed.
Root cause, found 2026-08-02: a real ESP-IDF bug — `i2c_master.c`'s NACK-handling busy-wait had no
timeout bound, so a NACK the hardware couldn't autonomously clear spun the calling task forever,
bypassing the app's own I2C timeout (matches upstream [espressif/esp-idf#17720](https://github.com/espressif/esp-idf/issues/17720),
fixed in IDF v5.5.4; this project is pinned to 5.5.0 with no PlatformIO upgrade path short of a
major-version jump). Patched at build time via `scripts/patch_i2c_master_nack_hang.py`.

Field verification: two consecutive sessions totaling ~10.5h of logged active runtime (~8h51m +
~1h48m), spanning an overnight standby/wake cycle and a real power-loss recovery (device dropped
mid-session). Log analysis (`system_1.log`/`system_2.log`) shows zero `TASK_WDT`/`PANIC` reset reasons,
an unbroken 60s `HEALTH` heartbeat cadence across both sessions (the freeze's signature is the
heartbeat silently stopping — never observed), and an I2C failure rate of ~0.01% in both, never enough
to trip the consecutive-failure recovery watchdog (ADR-0003). The one `POWERON` reset present lines up
exactly with the reported drop, not a firmware crash. Full analysis:
[`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md),
"Field Verification, 2026-08-05 — confirmed" section. Decision record for the fix itself:
[ADR-0021](docs/adr/0021-i2c-nack-hang-build-time-backport.md).

---

**System logger silently no-ops when enabled via the DEV/Settings toggle instead of at boot** —
invalidated an 8-9h FT-06 field-verification session (2026-08-03)

`system_logger::init()` — the only function that allocates the buffer/mutex and touches the file — was
called from exactly one place (`main.cpp`, boot only, gated on `settings.logging_enabled` already being
`true` in NVS at that boot). `dev_screen.cpp`'s logging toggle only called `system_logger::setEnabled()`.
Switching logging on via the toggle without rebooting left `g_initialized` false for the rest of the
session: `isEnabled()` reported YES, `flush()` returned success (it early-returns `true` when
uninitialized), and every `log()`/`logf()` call silently dropped. Discovered when an 8-9h field session
run to gather the first data point on the NACK-hang patch (see entry below) came back with a completely
empty `/sdcard/logs` — not truncated, never written at all. Fix: `logging_toggle_event()` now calls
`system_logger::init()` (idempotent) before `setEnabled()`, matching the boot-time path. FT-06 remains
**not field-verified** — the 2026-08-03 session doesn't count as an attempt. Full writeup:
[`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md), "SD Log Reliability
Hardening + Invalidated Field Test" section.

### Changed

**Waypoint cap raised 50 → 500; Haversine replaced with equirectangular approximation (2026-08-05)**

`RadarConfig::MAX_WAYPOINTS` (`include/ui/ui_manager.h`) was a hard load-time cap dating from before
the desc/hint→PSRAM migration (ADR-0001) — real geocaching.com pocket queries routinely exceed 50
caches and were silently truncated. Raised to 500, +63.3 KB SRAM / +562.5 KB PSRAM (static RAM now
60.4%, was 40.6%). Full cost/tradeoff analysis, including why 500 and not the 700 the SRAM budget
could otherwise afford: [ADR-0022](docs/adr/0022-waypoint-cap-raised-to-500-not-700.md).

Landed alongside the fix that made raising the cap safe: `drawWaypoints()` and `latLonToScreen()`
(`src/ui/navigation.cpp`) computed per-waypoint distance/bearing via Haversine — 10 double-precision
transcendental calls per waypoint, unconditional, on an ESP32-S3 FPU that's single-precision only.
Replaced with the equirectangular approximation (`dx = R·Δlon·cos(lat)`, `dy = R·Δlat`), accurate to
well under a pixel at radar scale: 2 multiplies + 1 `sqrtf`, plus `atan2f` only for waypoints that end
up off-screen (bearing is otherwise unused). Also dropped two now-unneeded `cos`/`sin` calls per
waypoint. Field-verified on hardware at the current real-world waypoint count — no regression.

`updateWaypointCountLabel()` (`src/ui/settings_screen.cpp`) color thresholds were hardcoded to 30/45
(tuned for the old cap of 50) and would have shown green at 490/500 waypoints loaded — now
proportional to `MAX_WAYPOINTS` (green ≤60%, yellow ≤90%, red above).

**Still open**: `wpt_us` at a real 500-waypoint load, and `loadAllGPXFiles()` parse time at that count,
haven't been field-measured yet — see ADR-0022.

### Added

**GPX manager shows the cache's friendly name; logs page gets select-all + bulk delete**

`src/gpx/gpx_server.cpp`: uploaded GPX filenames are normally bare geocache codes (e.g.
`GC38EVJ.gpx`), which tells the user nothing about which cache it is. `/list`'s JSON changed from a
bare array of filenames to `{"file":..., "name":...}` objects; a new `extractGpxName()` scans each
file (line-scoped, capped at 200 lines) for `<groundspeak:name>` inside the first `<wpt>` block,
falling back to the waypoint's own `<name>` (the GC code) if there's no groundspeak extension block —
matches the same tag preference `gpx_loader.cpp` already uses for `display_name`. The upload page now
shows the friendly name with the code as a small secondary line underneath. Separately, the logs page
(`/logs`) had no way to clear more than one file at a time; added a "Select all" checkbox and a
"Delete Selected" button that loops the existing per-file `DELETE /delete/logs/<filename>` endpoint
over the checked set — no new delete endpoint needed. Build impact: +672 bytes RAM (0.2 pts),
negligible flash.

---

**Per-boot SD log rotation + GPS-synced timestamps + reset-reason logging in system logger**

`system_logger.h`/`.cpp`: `init()` now rotates `system.log → system_1.log → … → system_5.log`
(`MAX_ROTATED_LOGS = 5`, oldest dropped) before opening a fresh file each boot, instead of appending
forever into one file a later boot's `MAX_LOG_SIZE` truncation could silently eat into — the boot that
matters now survives at least 5 power cycles. `getTimestamp()` prints uptime until the system clock is
GPS-synced (`ntp_sync.cpp` calls `settimeofday()` on fix), then switches to real UTC — same
`>= 2020-01-01` sanity gate `ntp_sync.cpp` already uses, so a session spanning a fix is self-consistent
and dateable without cross-referencing boot time. Every boot also now logs `esp_reset_reason()` decoded
to a string, to tell a real reset apart from a hang. Built alongside the fix above, for the next FT-06
field attempt. Full writeup: [`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md),
"SD Log Reliability Hardening + Invalidated Field Test" section.

Build impact: negligible (a few dozen bytes flash for the rotation/timestamp/reset-reason logic).

---

**FT-06 root cause found and patched: unbounded busy-wait in ESP-IDF's I2C driver after a NACK**

Reading the pinned ESP-IDF 5.5.0 source (`i2c_master.c`, `s_i2c_send_commands()`) directly found a
real driver bug: after an `I2C_EVENT_NACK`, the driver waits for the hardware to finish the STOP
condition with `while (i2c_ll_is_bus_busy(hal->dev)) { nop; }` — no timeout, no bound, completely
bypassing the `xfer_timeout_ms` the application passed in. If the bus never autonomously clears after
a NACK, the calling task hangs forever. This is a known, already-fixed upstream bug
([espressif/esp-idf#17720](https://github.com/espressif/esp-idf/issues/17720)) — fixed in IDF v5.5.4,
confirmed by diffing tagged source across v5.5.1 through v5.5.5. This project is pinned to 5.5.0;
PlatformIO's registry offers nothing between 5.5.3 (still broken) and 6.0.x (major-version jump, out
of scope). New `scripts/patch_i2c_master_nack_hang.py`, wired into `platformio.ini`'s `extra_scripts`,
backports just the upstream timeout-bound fix into the vendored driver on every build —
idempotent, portable (no hardcoded paths), and safe-by-construction (only patches an exact
byte-for-byte match; warns loudly and does nothing if the framework package ever changes underneath
it). Verified: patch applies, reruns correctly no-op, firmware builds clean. **Not yet field-verified**
against a real freeze — the trigger condition isn't reproducible on demand. Full writeup:
[`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md), "Root Cause Found and
Patched" section.

**TWDT now panics (resets) on timeout instead of only logging** — response to FT-06 occurrence 4

`src/core/main.cpp`: `wdt_config.panic_on_timeout` `false → true` (timeout stays 30s). The forensic
build below (100kHz + per-op logging) field-tested to a fresh freeze at 04:26 with `i2c_consec=0` and
zero further log output of any kind — strong evidence a System Task I2C call blocked past its own
driver timeout rather than returning a countable failure, which is invisible by construction to the
existing consecutive-failure wedge detector. Doesn't fix the underlying driver hang; converts a silent,
unrecoverable freeze into a self-healing reboot, and the next boot's `Reset reason: TASK_WDT` confirms
the mechanism if it recurs. Bus speed deliberately left at 100kHz — this hang is orthogonal to bus
speed/signal timing. Full writeup: [`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md),
"Occurrence 4" section.

**I2C forensic logging + 100kHz bus speed for FT-06 freeze investigation** — field-tested 2026-08-02;
did not prevent freezes (see entry above), but delivered the most complete trace of one captured so far

Per-op failure/latency logging (timestamp, device, register, error, calling task, duration), per-device
counters surfaced via `diag i2c`/60s SD heartbeat/on-wedge capture, unconditional SDA/SCL line-level
logging at every init/recovery, a 10s EXIO register canary, and a DEV-HUD I2C failure line — all
routed through `system_logger` so they survive the power cycle a freeze forces. Bus speed dropped
400kHz → 100kHz alongside it as a reversible mitigation experiment. Also fixed the `dev_mode` boot
banner, which had claimed verbose SD logging (UI checkpoints, button/queue events, 30s heartbeat) that
was never implemented — the real heartbeat ran every 120s with four fields, which is very likely why
the SD log looked "almost useless" in the field. Heartbeat is now 60s and carries I2C stats.
Full writeup: [`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md), "Forensic
Logging Build + 100kHz" section.

Build impact: RAM 40.4% → 40.6%, Flash +small (per-device stat structs + log call sites).

---

**Field data logging: accelerometer driver + CSV sample capture (WP-1)** — ✅ *verified on hardware,
2026-08-02 — field-test pre-flight checklist run, full START/STOP/back navigation chain exercised
repeatedly including auto-standby, no crash or hang*

The compass calibration work (`docs/compass_calibration_foundation.md`) is blocked on constants that
cannot be guessed — `H₀`, the tilt threshold, the accel gravity τ, the body-shake spectrum, and the
actual tilt bias while walking. This is the build that measures them.

**Why files and not serial**: the serial monitor requires USB, and USB also powers the 5V rail and
charges the battery, so a battery-powered field trip produces **no serial output at all**. Every
"walk around then read the log" approach is impossible. Samples land in CSVs and come off the device
over WiFi afterwards.

- **`compass_qmc5883l`** — now exposes the hard-iron-corrected vector (`cx/cy/cz`) and
  `h_mag = sqrt(cx²+cy²)`, the horizontal field magnitude the heading `atan2` was already computing
  and discarding. Held flat with a good calibration this is constant with heading, so departures from
  it separate tilt from a stale calibration from a magnetic disturbance. `cal_z_offset` is now
  applied; it is still 0 (a flat 360° spin cannot calibrate Z), and the heading formula stays 2-axis,
  so this changes no behaviour today — it makes the code correct ahead of WP-5.
- **`accel_qmi8658`** — new minimal driver for the QMI8658 already sitting on the shared I2C bus.
  Accel-only (`CTRL7 = 0x01`), ±4G/250Hz, one 6-byte burst at 10Hz from the System Task. **Not** a
  revival of `imu_sampling.cpp`: that did *gyro* heading fusion, which drifts by construction. This
  reads gravity, which does not. The gyro rides along in the same contiguous burst for logging only.
- **Read placement** — inside the existing compass gate chain in the System Task, so it inherits the
  WiFi-AP suspension and post-standby re-init those gates exist for. Reading it anywhere else would
  reintroduce exactly those failures.
- **`field_log`** — one CSV per sample, `/sdcard/logs/cal_<NNN>_<label>.csv`. The sensor tick pushes
  rows into a 512-row PSRAM ring; a dedicated Core 0 writer task drains it. No filesystem call ever
  happens on the sensor path — an SD write stalling would show up as jitter in the `ms` column,
  corrupting the timing data the samples exist to measure. Free-space and storage checks are cached
  by the writer for the same reason (`f_getfree` can walk the whole FAT, and the live readout polls
  from the UI Task).
- **Field Log screen** (DEV tab) — START/STOP, a label selector cycling the fixed §8.3 vocabulary, and
  live elapsed/rows/size/free readout. **Buzzer confirmation is not decoration**: half these samples
  are taken while tumbling the device or holding it phone-style, i.e. not looking at the screen.
  Chirp on start, double-beep on stop, rapid pulse on failure-to-start.
- **100Hz mode** for sample 9 — a short-lived task created only while a `shake-100hz` sample records
  and deleted on stop. `SYSTEM_UPDATE_MS` is deliberately untouched: it is the sensor clock for both
  the compass and the GPS gate, and the heading EMA τ is derived from it. This accepts the 100Hz bus
  risk for 30 seconds inside a controlled recording rather than architecturally, and I2C op/failure
  counts are written into every sample's header and footer so the cost is measurable per-sample.
- **`/logs` page** now lists `.csv` as well as `.log`, or the samples would exist on the card and be
  invisible — that page is the only way off the device without serial.
- Serial commands `accel [status|read|on|off|gyro on|off]` and `flog [status|start <label>|stop]`.
- Kill switch (`accel_enabled`, NVS-persisted, also on the Field Log screen) per the I2C risk
  assessment: the bus has a tuned timing floor with an undiagnosed cause (ADR-0013) and an open
  freeze issue (FT-06), so a sixth actively-read device must be instantly reversible without a
  reflash. Defaulted **on** — the likelier failure is collecting a whole trip with no accel data.

**⚠️ Corrected a wrong premise in the plan document**: it asserted `/sdcard` was the FFat mount. It is
not. `device_manager::initSD()` mounts a **physical SD card** via `esp_vfs_fat_sdmmc_mount`, and the
11.7MB `ffat` partition in `partitions_ota.csv` is never mounted by anything. **A card must be
inserted or there is nowhere to write.** The screen shows "NO SD CARD" and `startSample()` refuses,
but that is a thing to find at the desk, not at the trailhead.

**Known coupling**: the accel read sits inside `if (compass_ok)`, so a compass that failed to
initialise also silences the accel. Accepted deliberately — inheriting the gate chain is worth more
than independence, and a sample with no heading columns is mostly worthless anyway.

**Build impact**: RAM 132,392 → 132,640 B static (+248 B); flash 1,608,247 → 1,621,707 B (+13,460 B).
Plus, in DEV mode only, ~44KB PSRAM for the row ring and two task stacks (6KB writer, 4KB high-rate,
the latter only while a `shake-100hz` sample records).

**Code references**: `src/hardware/sensors/accel_qmi8658.cpp`, `src/utils/field_log.cpp`,
`src/ui/field_log_screen.cpp`, `src/utils/task_manager.cpp` (System Task sensor block,
`highRateTask`), `src/gpx/gpx_server.cpp` (`logs_list_handler`).
Plan and field protocol: [`docs/compass_calibration_foundation.md`](docs/compass_calibration_foundation.md) §8, §12.

**Compass calibration field trip + analysis (WP-2/WP-3) — go decision for tilt compensation**

The field trip ran 2026-08-01/02: 18 samples (`cal_006`–`cal_023`), all 9 planned labels plus two
unplanned rotate-left/rotate-right variants. Zero dropped rows across every file; 10Hz sampling held
σ≈0.11ms, the 100Hz high-rate mode held σ≈0.03–0.07ms with 0–1 I2C failures out of thousands of ops.

Offline analysis (pandas/numpy script, [`docs/calibration/analyze.py`](docs/calibration/analyze.py))
answered every question in §10 of the foundation doc that this trip was scoped to answer. The headline
result: **tilt-induced heading error is heading-dependent, not a fixed bias** — at ~46–50° tilt, error
vs GPS course was −135° walking north and +4° walking south from otherwise similar tilt angles. A 2-axis
compass (`atan2(cy, cx)`) at LA's ~59.7° magnetic inclination doesn't have one tilt error to correct,
it has one *per heading* — ruling out any lookup-table fix and confirming Level 3 (real
accelerometer-based tilt compensation) is required, not optional. Flat holding remains accurate in
every direction tested. Also answered: `H₀` ≈ 3000 (raw units, ~1% repeatable); a 60s freeform tumble
covers the full sphere (3-D calibration is feasible, unblocking WP-5); and accel-only suffices for
tilt compensation with no gyro needed, provided it's oversampled and averaged rather than read at a
flat 10Hz (the shake spectrum peaks at ~2Hz/~4Hz — walking cadence and its harmonic — with ~40% of
energy above a 10Hz sample's 5Hz Nyquist limit).

Two samples flagged "discard, unsure of hold orientation" in the field (021, 022) were recovered
instead of dropped — their accelerometer `az`/`ay` split matches the flat-hold and phone-style
signatures from the samples that weren't in doubt.

**Full numbers, tables, and the reproducible script**: [`docs/calibration/wp3_results.md`](docs/calibration/wp3_results.md),
[`docs/calibration/README.md`](docs/calibration/README.md) (per-sample manifest with field annotations),
[`docs/calibration/analyze.py`](docs/calibration/analyze.py). Results also folded back into §10 of
`docs/compass_calibration_foundation.md`.

**Compass Level 1 health metrics + trust indicator (WP-4)**

Runtime detection of a bad compass reading, using only data already read and previously discarded —
no new hardware, no bus cost. Built on the WP-3 field numbers (`H₀` ≈ 3000, healthy circle-fit band
2–4% residual / ~1.06–1.07 axis ratio, tilt inflates `h_mag` by ~23% at 45–50°).

- **Calibration overlay now captures three quality metrics from the same 360° sweep it already runs**:
  `H₀` (mean horizontal-field semi-axis radius), a circle-fit RMS residual (%), and the axis ratio.
  The residual is computed exactly from the aggregate raw x/y sums using the *final* offset — not a
  running per-tick approximation, which would bias early samples. Shown live during the sweep
  (`H0:.. R:..%  [GOOD/OK/LOW]`) in place of the old coverage-only readout, and persisted to NVS
  alongside the existing hard-iron offsets.
- **`compass_qmc5883l::classifyHealth()`** — a new runtime classifier: `UNCALIBRATED` (no `H₀` yet),
  `HEALTHY`, `TILTED` (h_mag elevated — tilt only ever inflates it, per the field data), or
  `DISTURBANCE` (sensor overflow only — see correction below). Smooths `h_mag` with a ~1s EMA and
  applies hysteresis around the tilt transition ratio (enter/exit at 1.12/1.08) so the state doesn't
  chatter at the ~3% noise floor — the same shape as the beacon proximity zone classifier.
  **This detects; it does not correct** — recovering true heading from a tilted reading needs the
  tilt axis, which is Level 3 (WP-6), not this.
  - **⚠️ Correction, same day**: the first version also guessed a low-magnitude threshold (ratio <
    0.85) for `DISTURBANCE`, reasoning a disturbance might weaken the field. That was never
    field-verified and reported as unreliable in use within hours (hit-or-miss "interference"
    warnings walking near metal objects) — and is probably backwards for the common case: a nearby
    ferromagnetic object concentrates field lines, which **inflates** `h_mag`, the same direction as
    tilt, not the opposite. Removed. `DISTURBANCE` is now sensor-overflow-only (a hardware fact, not
    a guess) until someone logs `h_mag` walking past a real disturbance and derives an actual
    threshold.
- **Radar HUD trust indicator** — a new HUD label, hidden when healthy, that surfaces "Compass: hold
  flat" / "Compass: interference" / "Compass: recalibrate?" / "Compass: not calibrated". The
  recalibrate prompt comes from the *stored* calibration's own quality score (residual > 5% or axis
  ratio outside ~0.83–1.20), not from live dynamics — distinguishing a genuinely stale calibration
  from a momentary tilt from a single live reading isn't something the field data supports doing
  reliably, so Level 1 doesn't try.
- **`compass cal` serial command** now prints `H₀`, residual, axis ratio, and the live classification
  alongside the pre-existing offset-magnitude quality heuristic. `compass cal set X Y` (manual offset
  override) carries the existing quality metrics through unchanged rather than resetting them, since
  a manual override isn't a scored sweep.
- **Build impact**: +344 bytes RAM (132,384 → 132,728, 40.5%), flash 77.6%.
- **⚠️ One-time migration note**: a calibration saved before this feature existed has no `H0`
  baseline (`compass_cal_h0 == 0` in NVS), so the HUD shows "Compass: recalibrate?" once after
  updating, even though the existing offsets still work. Recalibrating once (Settings > Display)
  clears it and populates `H0`/residual/axis-ratio. See `docs/compass.md` "When to recalibrate".

Files: `include/hardware/sensors/compass_qmc5883l.h`/`.cpp`, `include/settings_manager.h` +
`src/utils/settings_manager.cpp` (three new NVS-backed fields), `src/ui/settings_screen.cpp`
(calibration overlay), `include/ui/ui_manager.h` + `src/ui/ui_manager.cpp` (HUD label), `src/ui/navigation.cpp`
(`updateRadarDisplay()`), `src/utils/diagnostics.cpp` (`compass cal`/`compass cal set`).
Design: [`docs/compass_calibration_foundation.md`](docs/compass_calibration_foundation.md) §5, §12 (WP-4).

**Compass Level 2: 3-axis calibration (WP-5)**

A flat 360° spin cannot calibrate Z — the axis never changes what it points at during the spin, so
`min ≈ max` and the offset is unrecoverable. Field sample 8 (`freeform`,
`docs/calibration/wp3_results.md`) had already shown a tumble/figure-8 motion covers the sphere on
this hardware (elevation −87.6°..+82.5°, azimuth the full −180°..180° in ~60s); this build turns that
feasibility result into a real `cal_z_offset`.

- **Calibration overlay is now two-phase.** Step 1 is the original flat 360° spin, byte-for-byte
  unchanged — same X/Y min/max, same `H0`/circle-fit residual/axis ratio (WP-4). Step 2 is new: a
  tumble/figure-8 instruction, min/max on Z, gated on **3-D coverage** rather than a timer alone. The
  Next/Save button relabels itself between the two roles instead of a separate control.
- **3-D coverage needs no accelerometer.** Elevation (`asin(cz/|m|)`) and azimuth (`atan2(cy,cx)`) are
  computed from the corrected magnetometer vector in **sensor frame** — the same quantities WP-3's
  offline analysis used to confirm sample 8's coverage. Step 2 unlocks Save only once Z span,
  elevation span, and azimuth sector count (8 sectors of 45°) all clear their OK/GOOD thresholds,
  mirroring the tiers X/Y already used — a tumble that only rocks side-to-side won't pass on Z span
  alone.
- **Min/max per axis, not an ellipsoid fit.** WP-3's own flat-sample axis ratios (1.06–1.17) said soft
  iron is minor on this hardware; a 3×3 soft-iron matrix would be new, unvalidated math for a
  correction the field data says is secondary. See ADR-0019 for the full reasoning, including why the
  flat-spin baseline stays a separate step rather than being derived from the tumble.
- **`compass_qmc5883l::read()` needed no change** — it has applied `cal_z_offset` since WP-1
  (`compass_qmc5883l.cpp`), always 0 until now. The 2-axis heading formula is unchanged; consuming
  `cz` is Level 3 (WP-6).
- **Build impact (measured)**: RAM 132,728 → 132,760 bytes (+32, 40.5%), flash 1,627,955 → 1,629,399
  bytes (+1,444, 77.7%).

Files: `src/ui/settings_screen.cpp` (calibration overlay — phase state machine, tumble tracking, Save
callback), `include/hardware/sensors/compass_qmc5883l.h`/`.cpp` (comments only — code already
supported a real Z offset). Design: `docs/compass_calibration_foundation.md` §12 (WP-5).
Decision record: [ADR-0019](docs/adr/0019-3-axis-tumble-calibration-not-ellipsoid-fit.md).

**Compass Level 3 groundwork: Tilt Bench capture screen (WP-6, in progress)**

WP-6 needs the fixed rotation between the magnetometer's and accelerometer's sensor frames — they sit
on different PCBs, so this is a hardware fact, not something derivable from the mag-only field data
already collected. Rather than guess it (the exact "un-instrumented constant" mistake this project has
made before — see "The residual trap" in `docs/performance_optimization_backlog.md`), added the tooling to measure it directly
on the 6-pose bench protocol in [`docs/compass_tilt_bench.md`](docs/compass_tilt_bench.md).

- **New `tilt_bench` module + Settings > DEV > Tilt Bench screen** — Start Session opens a CSV on the
  SD card, CAPTURE takes one synchronized accel+mag reading and appends it tagged with the current
  pose (auto-advancing through the 6-pose list), End Session closes the file. Retrieved over WiFi from
  `/logs` afterward, same as `field_log`.
  **Deliberately not the serial `compass tiltbench` command this started as**: USB is required for
  serial, but it makes the NOSE-UP/DOWN and EDGE-DOWN poses awkward to hold cleanly, and a
  cable/charger is itself a plausible local magnetic-field source right next to the magnetometer —
  the same contamination risk `field_log` was built to avoid (§8.1). The serial command still exists
  as a quick desk-side sanity check with USB already attached, but is not the tool for the actual
  protocol.
- **Unlike `field_log`, no ring buffer/writer task** — captures are user-triggered and rare (a
  handful per session), so a direct `fopen`/`fwrite`/`fflush` from the UI Task on the CAPTURE tap is
  the same class of occasional blocking write already accepted elsewhere (e.g. the calibration
  overlay's NVS save).
- **Build impact (measured)**: RAM 132,760 → 132,872 bytes (+112, 40.5%), flash 1,630,259 →
  1,634,035 bytes (+3,776, 77.9%).

Not yet run on hardware; no heading-formula change yet. Files: `include/utils/tilt_bench.h`,
`src/utils/tilt_bench.cpp`, `include/ui/tilt_bench_screen.h`, `src/ui/tilt_bench_screen.cpp`,
`src/ui/dev_screen.cpp` (entry point).

**Compass Level 3 tilt compensation implemented (WP-6)**

Bench data came back (`docs/calibration/tiltbench_001.csv`, 2 passes × 6 poses) and was analyzed to
derive the mag↔accel frame rotation and a working heading formula — see
[ADR-0020](docs/adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md) for why the textbook
roll/pitch formula and the first-principles sign guess were both tried and rejected in favor of a
vector cross-product formula and a bench-derived sign.

- **New `tilt_compensation` module** (`include/navigation/tilt_compensation.h`,
  `src/navigation/tilt_compensation.cpp`): a τ-EMA gravity estimate from the accelerometer, and
  `computeHeading()` — normalizes gravity to "down", builds an "east/north" horizontal basis via
  cross products against the device's Y axis (physical top of screen), projects the frame-rotated mag
  vector onto it, `atan2`. Derived signed permutation: `device_x = cy, device_y = cx, device_z = cz`,
  no extra negation. Returns invalid (falls back to the existing 2-axis heading) when the device is
  pointed close to straight up/down — the reference axis is then parallel to gravity, a genuine
  singularity of this formula, not a data or calibration defect.
- **Wired into the System Task's compass pipeline** (`src/utils/task_manager.cpp`): tilt-compensated
  heading feeds into the existing declination/smoothing pipeline in place of the 2-axis heading
  whenever it's valid and enabled, so nothing downstream (WMM declination, the heading EMA) needed to
  change.
- **New `compass tilt` diagnostic commands** (`src/utils/diagnostics.cpp`): `compass tilt` (live status
  — enabled/sign/gravity validity/both headings side by side), `compass tilt on|off` (kill switch),
  `compass tilt sign +1|-1` (runtime sign flip) — all session-only, not NVS-persisted, so the sign can
  be corrected in the field without a reflash if a live check ever shows a steady ~180° offset from a
  known heading.
- **Field test (2026-08-02): gravity EMA tau lowered 1.0s → 0.5s.** A fast flat→nose-up tilt showed a
  real, ~30°, fast-recovering transient heading bounce during the transition itself — the gravity
  estimate lagging the near-instantaneous mag reading while still converging on the new orientation.
  This is the accel-only tilt compensation limitation [ADR-0018](docs/adr/0018-tilt-compensation-required-gyro-deferred.md)
  already flagged as the gyro trigger condition; the response here was to tighten the EMA rather than
  add gyro fusion, since the bounce is bounded and recovers within about a second — see ADR-0018's
  2026-08-02 update. `GRAVITY_EMA_TAU_S` was never fit to a walking-shake sample in the first place
  (comment in `tilt_compensation.cpp`), so 0.5s is confirmed-by-field-test, not a final answer either.
- **Build impact (measured)**: RAM 132,872 → 132,888 bytes (+16, 40.6%), flash 1,634,035 → 1,636,495
  bytes (+2,460, 78.0%).

**Sign confirmed in live use (2026-08-02)**: hand-held testing (flat, nose-up, recovery) showed the
heading settling back to the correct value each time, not a steady ~180° offset — the bench-derived
sign (`+1`, no extra negation) is correct as shipped. `compass tilt sign +1|-1` stays available as a
runtime revert path regardless, per this project's standing rule of keeping empirically-fixed signs
field-adjustable.

### Fixed

**Field Log pre-flight checklist found three bugs before the trip, not during it (WP-1
verification, 2026-08-02)** — ✅ *all three verified fixed on hardware*

Running the checklist itself (rather than trusting "builds clean") surfaced three independent bugs,
two of them severe enough that the outdoor trip would have produced no usable data or a bricked
session.

1. **CSV files never saved; START immediately auto-stopped; 3rd START rebooted the device.** Root
   cause: `CONFIG_FATFS_LONG_FILENAMES=y` in `sdkconfig.defaults` is not a real leaf option — it's the
   Kconfig **choice group name**, so setting it `=y` is silently ignored and the build kept
   `FATFS_LFN_NONE=y`. Every `fopen("/sdcard/logs/cal_001_flat360.csv", "wb")` was therefore being
   evaluated against strict 8.3 short-name rules and failing. Fix: the actual selectable symbol is
   `CONFIG_FATFS_LFN_HEAP=y` (heap over stack — the LFN buffer, up to `FATFS_MAX_LFN` chars, would
   otherwise land on already-tight task stacks like `field_log`'s 6KB writer). Verified via a full
   `sdkconfig.cc-radar` regeneration + diff (PlatformIO does not regenerate it from
   `sdkconfig.defaults` automatically — see the sdkconfig note in the Render Pipeline section of
   CLAUDE.md) showing exactly the intended change and no drift elsewhere.

2. **Two LVGL crashes and one hang, all in `navigation::goToSettingsScreen()`, all the same root
   cause wearing different masks.** `LV_MEM_SIZE` is only 64KB (`include/ui/lv_conf.h`) and a full
   settings screen is ~6 tabs of widgets; LVGL doesn't check allocation failures at every call site
   (e.g. `disp->screens[]`'s unchecked `lv_mem_realloc()`, `lv_obj_class.c:70`). Every failure traced
   to **two full settings-screen trees being alive in LVGL's pool at the same time**:
   - *Crash #1* (`LoadProhibited` in `lv_obj_get_local_style_prop`): the outgoing screen was deleted
     while it was still the *active* screen (a long-press-to-settings firing while already on
     Settings — real when touch is dead and the user falls back to the physical button), corrupting
     LVGL's active-screen bookkeeping. First fix: defer that delete until after the replacement
     loads.
   - *Crash #2* (`LoadProhibited`, near-null, inside `lv_label_create` while populating the DEV tab):
     with crash #1's fix in place, `settings → field log → back` left the outgoing screen alive while
     a second full tree was built on top of it. Second fix: delete the outgoing screen immediately,
     before building the replacement, whenever it is *not* the active one.
   - *Hang* (no panic — `UI_Task not responding`, loop count frozen, health-monitor recovery
     exhausted its 3 attempts): the two fixes above were individually correct but structurally in
     tension — the deferred-delete branch from fix #1 still built the replacement tree while the old
     *active* one was alive, reproducing the exact condition fix #2 had just eliminated on the other
     path. This time the pool corruption landed in the allocator's internal free-list instead of a
     data pointer, so `lv_mem_alloc()` spun forever walking a corrupted list instead of crashing.
     Diagnosed live via `task status` (I2C/Network/System tasks all healthy, only UI_Task failed —
     ruling out an I2C freeze or general heap exhaustion; `memory report` showed 6.4MB heap / 6.3MB
     PSRAM free, confirming the exhaustion was in LVGL's separate internal pool, not the general
     allocator). **Final fix**: never build a replacement while any old settings tree is alive,
     including the active-screen case — bounce off the always-valid radar screen as a momentary
     placeholder first (load radar → delete old tree → build new tree → load it). Nothing between the
     two `lv_scr_load()` calls yields to `lv_task_handler()`, so the radar screen is never actually
     flushed to the panel — no visible flicker.

3. Incidentally, this pass also captured the first evidence in weeks for **FT-06 (I2C bus freeze)**
   that isn't a probe-script artifact: a runtime NACK burst on the compass (an established, previously
   working device — not a boot-time probe) that also took down touch and the buzzer since they share
   the bus, and on the following reboot `[I2C] Bus reset OK` / `[I2C] EXIO recovered after clock
   pulses`, confirming the bus was genuinely electrically wedged across the reboot. This does not
   change FT-06's status (still unfixed, recovery code already in place) but does partially reopen the
   2026-07-31 finding that the earlier evidence was purely a probe artifact — see ROADMAP.md.

**Code references**: `sdkconfig.defaults` (FATFS section), `src/ui/navigation.cpp`
(`goToSettingsScreen()`).

**Heading smoothing re-derived as a time constant — the radar no longer bounces while walking
(WP-0.2)** — ✅ *verified on hardware: "shakiness while walking is way better"*

`HEADING_SMOOTHING` went from **α = 0.8 at 1 Hz** to **α = 0.3 at 10 Hz** when the compass rate was
raised. Those look like a deliberate re-tune but are not equivalent: converting each to a time
constant, `τ = −Δt / ln(1−α)`, gives **0.62 s → 0.28 s**. The filter ended up doing **2.2× less
smoothing in time terms** than the behaviour it replaced, and the heading visibly bounced.

**Fix**: α = 0.15, restoring τ ≈ 0.62 s at 10 Hz. The comment now states τ rather than only α, because
α alone silently changes meaning the moment the sample rate moves — which is exactly what happened
here, and is this project's most frequently repeated defect (a constant re-derived by rate but not by
*meaning*).

Also corrected the adjacent render-deadband rationale in `task_manager.cpp`, which did its arithmetic
from α = 0.3. With α = 0.15 a single-sample ±2° noise excursion is attenuated to ~0.3° rather than
~0.6°, so the 0.5° deadband gained margin rather than losing it — no code change needed there, but the
stale number would have misled the next reader.

This addresses **body shake only**. It does nothing for the tilt error (see the compass calibration
entry in ROADMAP.md): tilt is a *bias*, which no amount of smoothing removes.

**Build impact**: RAM 132,392 B (±0), flash 1,608,235 → 1,608,247 B (+12 B, alignment noise — the
change is one constant and comments).

**Code references**: `include/ui/navigation.h` (`HEADING_SMOOTHING`),
`src/utils/task_manager.cpp` (`COMPASS_UPDATE` deadband comment).
Analysis: [`docs/compass_calibration_foundation.md`](docs/compass_calibration_foundation.md) §9.1a.

### Changed

**GPS UART drained in chunks instead of one syscall per byte (backlog §8.3)** — ⏳ *not yet verified
on hardware*

`gps_bh880::read()` looped `uart_read_bytes(GPS_UART, &c, 1, 0)` — **one syscall per byte**, each
taking the UART driver's ring-buffer lock. At 115200 baud with 10Hz NAV-PVT that is on the order of
1–3k locked calls/sec on Core 0, for ~1000 bytes of actual data.

**Fix**: read up to 256 bytes at a time into a stack buffer and step through it. 256 covers a whole
NAV-PVT frame (100 bytes on the wire) plus slack in one call, so the common case is a single syscall
where it was ~100; the loop refills while more is queued, so a backlog still drains fully in one
`read()`. **The UBX state machine is untouched** — it is still byte-wise, and a chunk boundary can
fall anywhere inside a message without affecting parsing, since parser state lives in statics across
calls already.

This is Core 0 only — it never touches render, audio or BLE. Verification is correspondingly narrow:
still acquires a fix, satellite count normal, heading still tracks.

**Build impact**: RAM 132,392 B (±0 static; +256 B transient on the System Task's 8KB stack),
flash 1,608,147 → 1,608,235 B (+88 B).

**Code references**: `src/hardware/sensors/gps_bh880.cpp` (`read()`).

### Documentation

**Hygiene pass — stale comments and config that described a different system (backlog §4.1/§4.2)**

Comments and docs only; **no behaviour change** (flash moved +8 B, alignment noise from the LVGL
rebuild). Every item was a place where a reader would have been actively misled:

- **`CLAUDE.md` build config** — documented `[env:cc-moat-port]`, `framework = arduino` and
  Arduino-only build flags. The build is `[env:cc-radar]`, `framework = espidf`. Replaced, with a
  pointer that `sdkconfig.defaults` also governs behaviour.
- **`CLAUDE.md` display section** — marked **SUPERSEDED** with a correction box. Three claims were
  false: "ESP-IDF doesn't support bounce buffer" (a 10-line/18.75KB SRAM bounce buffer is active),
  "`full_refresh = 0` / partial refresh" (it is `1` in the default TILED mode and must be), and
  "software rotation" (replaced by the tiled transpose). Also "40-line bounce buffer" → 10-line.
- **`CLAUDE.md` heading source** — still described GPS/NMEA-RMC heading fusion with a 0.5-knot
  threshold and 1Hz updates. The compass is the sole heading source at 10Hz; GPS is UBX, not NMEA,
  and supplies position only.
- **`CLAUDE.md` waypoint perf** — "<2ms for 50 waypoints" was never measured and the instrumented
  figure is ~5ms. Replaced with a pointer to `wpt_us` on the `perf` HUD.
- **`system_config.h`, `task_manager.cpp`** — GPS sampling commented as 5Hz; it is 10Hz. Also
  `STABLE_SAMPLES = 5`, whose real meaning silently halved (~1s → ~0.5s) when the rate doubled —
  left at 5 (it only debounces a log line) but now says so, with a note on when a sample count
  *must* be converted to a duration.
- **`task_manager.cpp` CDC comment** — justified a log throttle with an unbounded USB CDC stall.
  There is no such stall (`cdc_acm_fifo_fill` rolls back and returns 0 rather than waiting). The
  throttle is still right, for the plain reason; the stated mechanism was fiction.
- **`task_manager.cpp` vsync gate** — documented as pacing the render. It does not, at an ~85ms
  frame: a binary semaphore given every 26.6ms is already signalled, so the take returns immediately
  and the loop free-runs. Now says so, plus why a counting semaphore would be worse.
- **`lv_conf.h` `LV_DISP_DEF_REFR_PERIOD 10`** — reads like a 100Hz request on a 37.7Hz panel. Kept
  at 10, with the LVGL source verified before leaving it: `lv_hal_disp.c:195` shows it only gates how
  soon a refresh may *start* after an invalidate, and `lv_anim.c:59` shows the same macro retimes
  every animation — the real reason not to "fix" it.
- **`platformio.ini` `flash_mode = dio`** — reads as contradicting `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`.
  It doesn't: that only governs the ROM loader's initial load, and the 2nd-stage bootloader upgrades
  to QIO at runtime. Annotated so it isn't "fixed" into a real bug.
- **`partitions/partitions.csv`** — orphaned pre-OTA 3MB/10MB table, unreferenced since the build
  moved to `partitions_ota.csv`. Header now says so. (Deletion was intended but blocked by a
  permission prompt.)

### Fixed

**`diag i2c` reported 61 phantom devices — the scan was lying, and it had misled a freeze
investigation** — ✅ *verified on hardware: 61 → 6*

`scanBus()` probed all 126 addresses with back-to-back `i2c_master_probe()` calls. Those return a
false `ESP_OK` on **alternate** calls, so the scan reported ~61 devices on a bus that has five.

**The proof is the address list, not a theory.** Hits landed on every second address —
`0x0A 0x0C 0x0E 0x10 0x12 ... 0x7C 0x7E` — with occasional phase slips, and the *set changed between
consecutive scans* while the total stayed at 61. Real devices appeared only when the phase happened
to align (0x51 in one scan, 0x15 in another, neither in both). No physical bus produces a strict
alternation; that pattern is an artifact of the probe loop. Captured on a healthy, freshly
power-cycled device whose touch, zoom and beacon all worked normally.

**Fix**: a 2 ms settle delay between probes so each starts from an idle bus, plus **double
confirmation** — a real slave ACKs every time it is asked, a phantom only on the alternating beat, so
requiring two consecutive ACKs rejects it. The raw hit count is printed alongside the confirmed one,
so if the artifact ever returns the scan says so instead of lying again.

**Measured on hardware**: `Found 61 device(s)` → `Found 6 device(s)` — 0x0D compass, 0x15 touch, 0x20
EXIO, 0x51 RTC, 0x6B IMU (0x7E is a reserved address and a residual artifact), with **56 single-ACK
hits rejected**, matching the prediction.

**Why this matters beyond the diagnostic**: this scan's output was part of the evidence for the
"recurring freeze = wedged I2C bus" root cause. That leg is now void. Combined with the second
finding below, the wedged-bus diagnosis should no longer be treated as established — see ROADMAP FT-06
and the backlog work queue.

**Also established this session (no code change)**: `[I2C] Bus reset OK` proves nothing on this chip.
Read from the IDF 5.5 source: `i2c_master_bus_reset()` → `s_i2c_hw_fsm_reset(clear_bus=true)` →
`s_i2c_master_clear_bus()`, which on ESP32-S3 (`SOC_I2C_SUPPORT_HW_CLR_BUS=1`) waits with
`while (i2c_ll_master_is_bus_clear_done(hal->dev))` — and that function is hardcoded
`return false; // not supported on esp32s3`. The wait loop never runs; the call returns `ESP_OK`
without waiting for or verifying the clear. Every recovery path we ship rests on this primitive.

**Build impact**: RAM 132,392 B (±0), flash 1,607,995 → 1,608,155 B (+160 B).

**Code references**: `src/hardware/i2c/i2c_manager.cpp` (`scanBus()`).

**I2C bus freeze — recurring full-interface lockup, self-heal recovery added** — ⏳ *fix built and
committed; effectiveness monitored in the field rather than verified, since a stuck bus can't be
safely reproduced on demand*

Reported twice in one session: button, touchscreen, and display updates all stopped responding.
First occurrence needed a full power cycle; the second self-cleared when the device went to standby
and woke back up.

**Root cause, confirmed rather than assumed**: a stuck I2C bus. A slave (CST820 touch, PCF85063 RTC,
or TCA9554 EXIO) left mid-transaction holds SDA low indefinitely — an MCU-only reset doesn't power-cycle
external chips, so it can't free them — and every subsequent transaction to every address fails until
the bus is explicitly clock-recovered. The second occurrence is the confirming evidence, not just the
theory: wake-from-standby calls `i2c_manager::reinit()` as part of its own recovery sequence
(`standby_manager.cpp`), and that's what cleared touch/button/sound. The fix below generalizes that
same, already-proven recovery primitive into something that triggers proactively.

**Three changes**:
1. **New `i2c_manager::Stats::consecutive_failures` counter** — increments on every failed
   `read()`/`write()`, resets to 0 on success. A wedged bus fails *every* transaction to *every*
   address, so a sustained run here (not one device's occasional miss, which clears on its next
   success) is the signal a watchdog can act on.
2. **Boot-time self-heal** in `i2c_manager::init()` — when the initial EXIO ping fails, run a clock
   recovery (9 SCL pulses) and retry once before giving up, instead of warning and continuing
   crippled. This also covers the previously-documented boot-hang case in `docs/troubleshooting.md`,
   and any reboot that follows a runtime wedge — previously the device would come back up still
   jammed, since an MCU reset can't free a slave that's holding the line.
3. **Runtime watchdog** `checkI2CBusHealth()` in System Task (~10Hz) — at ≥10 consecutive failures
   (under 1s of a fully jammed bus, given touch's ~11.7Hz poll rate), calls `i2c_manager::reinit()`.
   2s cooldown between attempts, gives up after 5 with an `UNRECOVERABLE` log rather than retrying
   forever against genuinely dead hardware.

**Separately found alongside this, not fixed**: after the self-recovered freeze, the on-screen
DEV/perf HUD label stayed frozen at stale values even though touch/button/sound/rotation all came
back. The first hypothesis was a dangling `ui.perf_label` LVGL pointer (its update is gated by
`lv_obj_is_valid()`, which would silently no-op forever with no error), but a follow-up read makes
that look weak: the label is created once on the radar stage (`ui_manager.cpp:311`) and nothing
deletes it — `lv_obj_del` appears only for the standby screen and the WiFi modals. Not root-caused or
fixed; `dev off` / `dev on` on the next recurrence discriminates between the pointer theory and a
render-path stall.

**Build impact**: RAM 132,384 → 132,392 B (+8 B), flash 1,607,099 → 1,607,967 B (+868 B).

**Code references**: `include/hardware/i2c/i2c_manager.h` (`Stats::consecutive_failures`),
`src/hardware/i2c/i2c_manager.cpp` (`init()`, `read()`, `write()`), `src/utils/task_manager.cpp`
(`checkI2CBusHealth()`, called from `systemTask()`).

**Full analysis**: [ADR-0003](docs/adr/0003-proactive-i2c-bus-recovery-watchdog.md) (why these
thresholds, and the alternatives rejected) · ROADMAP.md → FT-06 ·
[`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md) → work queue
§0 (what to watch for in the field).

### Changed

**Waypoint `desc`/`hint` moved from SRAM to PSRAM — frees ~64KB static RAM** — ✅ *verified on
hardware, no regressions*

`g_ui_state` was the largest symbol in the firmware (70,992 B, ~37% of static RAM), almost all of it
`desc[1024]` + `hint[256]` × 50 waypoints — read in exactly one place (`waypoint_screen.cpp`, one
waypoint at a time) but resident for all 50 permanently. Added a `WaypointDetail` struct holding just
those two fields, allocated once as a single block via `heap_caps_calloc(MAX_WAYPOINTS,
sizeof(WaypointDetail), MALLOC_CAP_SPIRAM)` in `ui_manager::init()`; `Waypoint::desc`/`hint` became
`char*` pointers into it instead of embedded arrays. No section attributes (`.ext_ram_noinit`
boot-crashes on this IDF); allocation failure is handled by leaving the pointers `nullptr` rather than
crashing, guarded at both the one write site (`gpx_loader.cpp`) and the one read site
(`waypoint_screen.cpp`). See [ADR-0001](docs/adr/0001-waypoint-detail-psram-cache.md) for why PSRAM
caching was chosen over re-reading the GPX file on tap.

**Flash dropped too, not just RAM — confirmed via `readelf`, not assumed.** `g_ui_state` has some
non-zero-initialized fields (e.g. `current_zoom`'s default enum value), so the whole object —
including the large all-zero `desc`/`hint` regions — was being placed in `.dram0.data` (a PROGBITS
section: its zero bytes are stored as literal zeros in flash and copied to RAM at boot) rather than
the free `.dram0.bss`. A `git stash` + `readelf -S` diff on the ELF showed `.dram0.bss` byte-identical
before/after; the entire saving came out of `.dram0.data`, which is why shrinking this struct paid off
in both partitions at once — not a coincidence, and not attributed without checking.

`MAX_WAYPOINTS` is still 50 — this frees the headroom but does not itself raise the cap; that remains
a separate follow-up (see ROADMAP).

**Build impact**: RAM 195,984 → 132,384 B (59.8% → 40.4%), flash 1,670,831 → 1,607,099 B
(79.7% → 76.6%).

**Code references**: `include/ui/ui_manager.h` (`WaypointDetail`, `Waypoint::desc`/`hint`),
`src/ui/ui_manager.cpp` (`init()` allocation), `src/gpx/gpx_loader.cpp` (write site),
`src/ui/waypoint_screen.cpp` (read site).

### Fixed

**Beacon sonar was choppy — the continuous tempo and trend-beep from the previous entry both had a
self-inflicted noise problem** — ⏳ *fix built, not separately re-verified by ear*

Field report right after the priority/continuous-tempo work below: "the beeping is choppy." Both
causes were introduced in that same change, and both are the exact defect being fixed everywhere else
that day — a discrete or noisy value driving something meant to be heard as continuous.

1. **Tempo was driven by `rssi_ema` (τ=0.5s).** RSSI wobbles ±3-5 dB standing still, and over the
   40 dB tempo-mapping span a 4 dB swing is a ~25% change in beat period. A continuous tempo only
   reads as a *glide* if the value driving it is itself smooth — otherwise "continuous" just means
   "jittering constantly" instead of "stepping occasionally," which is worse than the four-step
   version it replaced. Tempo now comes from `rssi_display` instead, and `DISPLAY_TAU_S` was raised
   1.0 → 2.0s. This makes the EMA split explicit as **decision vs presentation**: `rssi_ema` is fast
   because zone and trend have their own hysteresis/confirmation downstream, so latency hurts there
   and noise doesn't; `rssi_display` is slow because the ring and the sonar tempo are shown/heard raw,
   where noise is the entire problem — deliberately slower than the ring alone would want, because
   rhythm error is far more perceptible than visual lag.
2. **Beep length switched on the three-state `MovementTrend` enum.** Standing still, the regression
   slope hovers near zero, so the classifier flipped APPROACHING/STABLE/DEPARTING at random and the
   beep length jumped 60→30→12ms beat to beat — heard as the rhythm breaking up, not as information.
   Now interpolated continuously from the raw slope (`BeaconState::trend_slope_dbm_s`), saturating at
   ±2 dBm/s: 30ms neutral ±30ms, floored at 12ms.

**Build impact**: flash +116 B, RAM ±0.

### Fixed

**`I2C_PROCESS_MS` 20 → 10ms broke the button and buzzer — reverted** — ✅ *verified on hardware:
radar, beacon discovery and sound all confirmed back to normal at 20ms*

Attempted to halve the sonar's 20ms timing-quantization floor (backlog §8.1b) by doubling the I2C
Task's rate. On hardware: **button unresponsive, buzzer silent.** Reverted immediately.

The cost analysis behind the change was wrong in a specific and generalisable way — it counted the I2C
Task's own CPU cost (a non-blocking queue drain plus a few timestamp compares, genuinely trivial) and
never counted the actual contended resource: the **I2C bus**.

> **Correction, 2026-07-31**: this entry originally went on to explain the failure as *"the CST820
> touch driver calls `Wire` directly, bypassing `i2c_mutex`"*, citing `docs/compass_i2c_constraint.md`.
> **That mechanism is stale** — it describes the pre-ESP-IDF Arduino build. There is no `Wire` usage
> anywhere in `src/`or `include/`; `cst820_read()` goes through `i2c_manager::read()` under the
> recursive `g_bus_mutex` like every other device. The symptom and the revert are confirmed on
> hardware; **the cause is not known** and should be treated as un-diagnosed. See backlog §8.1b and
> `docs/compass_i2c_constraint.md` for the full correction.

**`I2C_PROCESS_MS = 20` is therefore a tuned floor, not an arbitrary constant**, and 20ms is a hard
limit on sonar timing resolution (~8% jitter at a 250ms interval) for as long as the buzzer is driven
over the shared bus. Marked void in the backlog with the reasoning, so it isn't retried. Beat
steadiness has to come from smoothing the *interval* (deadband + slew limit) or the RSSI *input*
(median filter before the EMA) instead — neither touches the bus.

**Separately discovered while debugging this**: a stuck I2C bus can hang boot completely, immediately
after `[I2C] Initialized: SDA=15...` — a slave (touch or RTC) left holding SDA low across an MCU reset
jams the bus, and only a **full power cycle** (not a reset) releases it, since the MCU reset doesn't
power-cycle the slaves. Confirmed on hardware and documented in `docs/troubleshooting.md`.

**Build impact**: RAM ±0, flash ±0 (constant reverted to its original value).

### Changed

**Beacon takes absolute priority, and its sonar became continuous + trend-aware** — ⏳ *priority logic
awaiting hardware verification; the tempo/trend-beep mechanics below were superseded within hours by
the choppy-sonar fix above (`rssi_display` instead of `rssi_ema`, continuous beep length instead of
the fixed 60/30/12ms), which is where the current behaviour is described*

Field report after the §7 rate work: the beacon appeared *silent*, and once that was explained, the
beeping was *"very difficult to gauge where to go"*. Two separate causes.

**1. A fixed waypoint could mute the beacon entirely.** `updateWaypointFixSonar()` called
`suppressSonar(true)` unconditionally as soon as a waypoint was fixed at 50 m zoom, and *then*
computed the tempo — so with the waypoint beyond 50 m the tempo was 0 → `stopSonar()`, leaving the
beacon permanently muted by something that made no sound itself. The continuous-tempo rewrite already
fixed that path, but the priority was still backwards.

**The beacon now wins outright.** A beacon is a thing you are trying to *find*; a fixed waypoint is an
area you are walking into, and its sonar is a secondary convenience. When `beacon_proximity::isInRange()`
becomes true the waypoint fix is **released**, not merely out-prioritised — leaving it fixed would keep
every other waypoint hidden from the radar and re-take the sonar the moment the beacon dipped out of
range. Safe against flicker: `isInRange()` reads the *confirmed* zone, which needs 1000 ms of
hysteresis-gated agreement to enter.

**2. The beacon sonar still had the four-step defect just removed from the waypoint sonar.** Tempo was
a `switch` on `state.zone` — 1500/750/500/250 ms — so most of a search happens *inside* one zone, where
moving produces no audible change at all. That is fatal for a beacon specifically: someone hunting is
not judging absolute loudness, they are listening for *change in response to their own movement*, and
a step function gives them none until they cross a boundary.

Tempo is now continuous and **linear in dBm** — 1500 ms at −90 dBm to 150 ms at −50 dBm,
`interval = 1500 · 0.1^((rssi+90)/40)`. Linear in dBm is the correct curve rather than an
approximation: RSSI ≈ C − 20·log₁₀(d), so equal dBm steps are equal *ratios* of distance — the same
geometric mapping the waypoint sonar uses over metres, reached from the other direction. The zone
still decides *whether* to beep; it no longer decides how fast.

**Trend is finally used for something.** It had been computed since the v2 redesign and read by
nothing. "Warmer/colder" is far more actionable than absolute level when hunting, because absolute
RSSI depends on the environment, the tag's orientation and your own body — but the *sign of the
change* is meaningful regardless. The buzzer is a bare on/off line with no pitch to modulate, but beep
*duration* is already a parameter, so this cost nothing: **60 ms** tone when APPROACHING, **30 ms**
neutral, **12 ms** clipped tick when DEPARTING.

**Build impact**: RAM ±0, flash 1,668,847 → 1,669,051 (**+204 B**).

**Beacon proximity: 2 Hz → ~5 Hz, and everything derived from that rate re-derived with it** — ✅
*verified on hardware 2026-07-31*: `beacon status` measured **4.24–4.37 Hz** live (mean gap ~230ms,
up from ~500ms), with `Scan callbacks` climbing at ~89/sec across ~30 nearby devices and the target
MAC count climbing alongside it.

Backlog §7 in full (7.3a–d). The BLE feed was capped at exactly **2.0 Hz** against a tag advertising
at 5 Hz, so about 60% of every advertisement was thrown away. Three independent causes, all removed:

1. **Duplicate filtering** — NimBLE reports a given advertiser to `onResult` once *per scan* while
   `filter_duplicates` is set.
2. **`g_pScan->stop()` on the first hit** — ended the scan the instant the beacon was seen,
   guaranteeing exactly one sample per cycle.
3. **A 500 ms scan/idle poll loop** in `update()`.

Now a **single continuous passive scan** (`start(0, …)` → `BLE_HS_FOREVER`), duplicates off,
`setMaxResults(0)`, 100 ms window == interval. The poll loop, `SCAN_INTERVAL_MS`,
`SCAN_DURATION_SEC`, `g_scan_in_progress` and the results-sweep block are all deleted.

**Passive is load-bearing, not just a power choice.** With active scanning and a legacy `ADV_IND`
advertiser, NimBLE *withholds* `onResult` until the scan response arrives, and failing that until the
scan completes. This had been filed as §7.3d "suspected, confirm with runtime logging" — reading
`NimBLEScan.cpp` confirms it outright, so the item closed without instrumenting anything.

**The more important half: every constant derived from the old rate was re-derived.** Left alone,
each would have silently changed meaning by 2.5–5× the moment the feed sped up — which is exactly the
defect (*a rate constant nobody re-derived after the pipeline around it changed*) that the audit was
named after.

- Both EMAs are **τ-based from measured elapsed time** (`α = 1 − e^(−dt/τ)`, τ = 0.5 s fast / 1.0 s
  display) rather than fixed per-sample α. Measured, because BLE advertising is lossy and a dropped
  advertisement must widen that sample's weight rather than skew the time constant.
- Zone confirmation is a **duration** (`ZONE_CONFIRM_MS = 1000`) rather than "2 consecutive samples",
  which meant 1.0 s only because samples arrived at 2 Hz.
- Trend slope is regressed against **real time in dBm/s** over a 4 s window rather than against sample
  index in dBm/cycle. Thresholds ±1 dBm/s, taken from the physics: `8.686·v/d` at 1.4 m/s and n = 2
  gives 0.6 dBm/s at 20 m, 1.2 at 10 m, 2.4 at 5 m. (Diagnostic only — nothing acts on trend.)
- `BEACON_LOST_TIMEOUT_MS` 15 s → 5 s, which at the new rate is already 25–50 missed advertisements.

**The ring is continuous now (§7.3c).** Width interpolates from `rssi_display` (−90 dBm → 6 px,
−65 dBm → 34 px) instead of snapping between four zone-selected widths. `rssi_display` — the slow EMA
that exists specifically to drive it — had been computed every sample and **read by nothing at all**.
The two decisions *around* the ring stay discrete and keep their hysteresis: whether to draw one, and
whether to switch to the solid CLOSE fill.

**Two footguns found and fixed while doing this**:
`setAdvertisedDeviceCallbacks(cb, wantDuplicates)` calls `setDuplicateFilter(!wantDuplicates)`
internally, so `debugScanAll()`'s restore path would have silently put the feed back to 2 Hz for the
rest of the session after any `beacon scan`; and with duplicates off `onResult` fires for every
advertisement from every device in range, where the old body built two heap `String`s per callback
before rejecting — it now compares a `NimBLEAddress` parsed once.

`beacon status` / `beacon trend` report the **measured** mean inter-arrival in ms and Hz — the direct
verification that this worked. **Still to check on hardware: radio power draw**, since 100% scan duty
is the one genuinely new cost. `SCAN_WINDOW_MS` is the single knob if it's objectionable.

**Build impact**: RAM 195,600 → 195,968 (**+368 B**, almost all the 48-entry timestamped trend ring),
flash 1,668,007 → 1,668,847 (**+840 B**).

**`Serial` no longer flushes on every call, and logging can be switched off** — ⏳ *awaiting hardware
verification*

Backlog §3.5 was filed as *reliability*: `fflush(stdout)` was believed to stall unboundedly on the
USB CDC path when no host is draining, blocking whichever task logged. **Checking the IDF 5.5 source
before implementing — the standing rule that has now caught four items — showed the premise is
false.** `cdcacm_write` → `esp_usb_console_write_buf` → `esp_usb_console_flush_internal` →
`cdc_acm_fifo_fill`, which rolls back and returns 0 when the host isn't draining. It silently drops
bytes; it never waits. The only `portMAX_DELAY` wait in that driver is on the read side, and its
`s_blocking` is false by default. **There is no stall on this console, in or out.**

The fix is still right, for a smaller reason. stdout is line buffered (IDF returns `S_IFCHR` from
`_fstat_r_console`), so the unconditional `fflush` was not merely redundant — it *defeated* the line
buffering: `print("x"); print(1); print("\r\n")` became three `cdcacm_write` calls where one would do,
and that function loops the VFS layer one byte at a time taking a recursive lock per byte.

- All nine unconditional `fflush(stdout)` calls removed. Explicit `Serial.flush()` remains.
- Added `SerialClass::setLogEnabled(bool)`, checked **before** formatting in every overload (the
  numeric ones gate before their `snprintf`). Default ON — nothing changes unless asked. Its value is
  field/battery mode, where no host is attached and every log line is wasted CPU.
- New `serial on | serial off` command. Input is never gated, so it still works with logging off, and
  `serial off` prints its confirmation before muting.

**Build impact**: **±0 flash, ±0 RAM** — deleting the nine flushes paid for the gate and command exactly.

### Added

**Panel ISR core probe (`perf`)** — ⏳ *result not yet read off hardware*

Backlog §1.5 hypothesises that the RGB panel's DMA/vsync ISR is installed on Core 1 — the same core
as `uiTask` — because `esp_intr_alloc()` binds to whichever core calls it and the panel is created
from the boot path, which sdkconfig pins to Core 1. This is a measurement, not a change:
`on_vsync_cb` records `xPortGetCoreID()` into a volatile (recorded rather than printed — `printf` is
not ISR-safe), and `perf` reports `Panel ISR: core N (uiTask on core M) — SHARED / separate`. One
reading either justifies moving display init to Core 0 or deletes the hypothesis.

**Build impact**: flash +264 B, RAM ±0.

**Waypoint sonar: continuous tempo, and it finally stops when you arrive** — ⏳ *awaiting hardware
verification*

The hysteresis fix below was verified working — and field testing in the same session showed it had
fixed the wrong layer. The report: the waypoint sonar "feels very chaotic, a lot of beeping even at a
further distance, is not as progressive as the beacon", and there was no way to stop it on arrival.
The ladder was steady and still wrong.

**1. Distance→tempo is now continuous, not four zones** (`navigation.cpp`). The old zones were
5/10/30/50 m — 5 m, 5 m, 20 m, 20 m wide — so the 10–50 m band where most of an approach happens was
only two tempi. You walked 40 m, heard one rate, one step, one rate. Now a geometric mapping,
**2000 ms at 50 m → 250 ms at 2 m**, `interval = 250 · 8^(ln(d/2)/ln 25)`. Equal distance *ratios*
give equal tempo *ratios*, so halving the distance always doubles the tempo and a constant walking
pace yields a steady acceleration rather than two plateaus and a lurch. The far end was deliberately
slowed past the 1500 ms originally proposed — "too much beeping far out" was half the complaint, and
the old ladder gave 750 ms at 30 m where the curve gives ~1420 ms.

**This replaced the zone hysteresis rather than building on it.** A continuous tempo has no rates to
flicker between, so the zone enum, the ±3 m hysteresis and the 1000 ms confirmation hold are all
deleted. The GPS-noise guard that replaces them is a **τ = 1.5 s EMA on the distance** using measured
`dt` (`α = 1 − e^(−dt/τ)`) rather than a sample count — the render-request rate is ~10 Hz with a fix
but is not guaranteed to be, and a sample-based EMA would silently change its time constant with it.
That is the right place for the guard: smooth the noisy input, not the derived output. One discrete
decision survives — beeping vs silent — so it keeps hysteresis: engage ≤ 50 m, release > 55 m.

**2. Arrival stop.** `Waypoint::found` already existed and was already set by tapping the fixed
waypoint within 15 m — the exact counterpart of tapping the beacon ball. `updateWaypointFixSonar()`
simply never read it, so arriving gave no way to silence the beeping short of unfixing the waypoint
or leaving 50 m zoom, while the beacon has silenced-on-found all along. It reads the flag now. Not
from the audit; this one surfaced from field use.

**Build impact**: RAM 195,600 (**±0**), flash 1,666,611 → 1,667,743 (**+1,132 B**).

### Fixed

**Sonar rhythm: the beat grid was walking, and the waypoint tempo had no hysteresis** — ✅ *verified
on hardware 2026-07-31* (the hysteresis half is now superseded by the continuous tempo above)

Found by a full-subsystem audit (2026-07-31) prompted by the question "did anyone check anything
other than the render?" — the answer was no; the whole optimization effort had been scoped to frame
time. Two independent defects, both audible, neither related to CPU speed.

**1. The sonar beat grid re-based off actual fire time** (`buzzer.cpp`). `sonar_next_beep_ms = now +
interval` used `now` — whenever `update()` happened to run, up to one I2C Task period (20 ms) late.
Every period was therefore `interval + (0..20 ms)`:

- per-beat jitter up to 20 ms — **8% at the 250 ms CLOSE interval**, well above the ~10 ms the ear
  resolves;
- tempo systematically **~4% flat**, because the grid absorbed the mean lateness instead of holding.

Now advances by a fixed step (`+= interval`) so the average tempo is exact, with a catch-up guard
that resyncs rather than firing backdated beeps if a stall puts it more than one interval behind.

**2. Waypoint proximity sonar had no hysteresis at all** (`navigation.cpp`). Distance→tempo was a
bare if/else ladder with hard boundaries at 5/10/30/50 m, evaluated at the 10 Hz sensor rate against
a GPS position that jitters ±2–5 m even with a good fix. Standing still near a boundary made the
tempo flip between two rates at random — a beep interval alternating between 500 and 750 ms sounds
broken.

Replaced with a zone state machine mirroring the one `beacon_proximity` already used for RSSI:
**±3 m hysteresis** on the exit threshold of the current zone, plus a **1000 ms confirmation hold**
before a zone change commits. Both guards are needed — hysteresis alone still flips on one large GPS
excursion, confirmation alone still flips on sustained jitter across the boundary. Zone state resets
on every disengage path so re-engaging never inherits a stale zone.

### Removed

**Blocking `buzzer::rapidPulse()`** — spun in `delay()` for ~60 ms and had no callers left after
`rapidPulseAsync()` superseded it. Called from the UI Task it would have stalled LVGL for most of a
frame.

**Build impact**: RAM 195,592 → 195,600 (**+8 B**), flash 1,666,247 → 1,666,611 (**+364 B**).

**Also documented, not yet implemented**: [`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md)
§7 (beacon BLE feed is rate-starved at 2 Hz against a 5 Hz source — not a CPU problem) and §8 (audit
of every remaining subsystem). Plus [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md)
— whether the device can tell you which way to walk. Short answer: BT 5.1 AoA is impossible on this
hardware, but body-shadow DF using the compass works, and is blocked on §7 because at 2 Hz a rotation
yields 1.7 samples per 30° bin.

### Performance

**CPU restored to 240MHz, and sensors raised to 10Hz now that the render outruns them**

Two changes that stop the render being faster than the things feeding it. **Frame 101.5 → 85.2ms
(1.19×), verified on hardware** — boot reports `[BOOT] CPU: 240 MHz`, all four tasks healthy, zero
I2C failures, no display artifacts.

| Stage | 160 MHz | 240 MHz | ratio |
|---|---|---|---|
| tiled rotate | 47.4 | 38.3 | 1.24× |
| radar bg fill | 21.5 | 20.5 | 1.05× |
| LVGL non-radar draw | 23.2 | 17.0 | 1.36× |
| radar paint | 9.4 | 9.3 | 1.01× |
| **FRAME** | **101.5** | **85.2** | **1.19×** |

Two predictions in the backlog were wrong in opposite directions, and both are worth carrying
forward: **rotate was not at the memory ceiling** (predicted "barely moves", delivered the largest
absolute gain — ~24% of the transpose was CPU work, not bandwidth), and **radar paint is not
CPU-bound** (predicted 1.5×, delivered 1.01× — it is bound by writing the draw buffer, not by
computing geometry, so optimizing the drawing math would buy nothing). "Full-screen PSRAM write"
turned out not to be one category.

*CPU 160 → 240MHz.* The binary had been running at 160MHz since the ESP-IDF migration: vanilla IDF
defaults to 160 and the Arduino core used to set 240 on our behalf, so the clock was lost silently
when the framework changed. One line in `sdkconfig.defaults`
(`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`). `CONFIG_PM_ENABLE` is off, so this is a fixed frequency,
not a DFS ceiling. Expect ~1.5× on CPU-bound work (BLE host, GPS UBX parsing, I2C, compass read,
LVGL draw and radar paint) and much less on the two full-screen PSRAM writes in the render, which
are bus-bound — a whole-frame estimate lands nearer ~80ms than 94/1.5. Cost is power draw; see the
battery measurement note in the backlog before treating this as settled.

Boot now prints the *measured* CPU frequency (`getCpuFrequencyMhz()` via `esp_clk_tree`), because
the only reason a 1.5× regression survived for months is that nothing ever printed it.

*Sensor rate 5 → 10Hz.* `SYSTEM_UPDATE_MS` 200 → 100. The System Task tick is the sensor clock: the
compass sub-timer (20ms gate) and the GPS gate (`GPS_UPDATE_INTERVAL_MS = 100`) both fire every
tick, so this doubles both. 10Hz is also the BH-880's native NAV-PVT rate, so GPS samples stop being
discarded, and `HEADING_SMOOTHING = 0.3f` was already tuned for a 10Hz compass it had never actually
received. Safe only because render requests are coalesced to at most one per UI Task loop.

- **Heading render deadband 1.5° → 0.5°** (`HEADING_RENDER_DEADBAND_DEG`). At 10Hz with EMA α=0.3 a
  single-sample noise excursion smooths to ~0.6°, so 0.5° still sits at the noise floor while a
  genuine 5°/s turn now redraws — under 1.5° nothing slower than 15°/s did. The threshold is close
  to vestigial as a load control either way: with a GPS fix, `RADAR_REFRESH` is queued every sample
  regardless, so it only gates anything indoors
- **Battery sampling explicitly pinned to 5Hz** in `systemTask`. `battery::update()` busy-waits
  ~1.5ms (15 ADC samples × 100µs) with no internal rate limit and was the only per-tick cost in that
  loop not already time-gated; everything downstream of it is 30s-gated, so doubling it bought
  nothing
- **The predicted input regression did not happen.** Verified outdoors with satellites locked:
  rotation and button both good. The concern was that with a fix, `RADAR_REFRESH` is queued every GPS
  sample, so the UI Task renders nearly every loop and polls input once per ~90ms instead of once per
  26.6ms vsync. That is still true — but ~90ms is comfortably shorter than a real button press (the
  boot log shows 132–186ms press durations), so nothing is missed and the latency stays under the
  perceptual threshold for a discrete action. The prediction conflated *poll interval* with *perceived
  latency*; for discrete input they are not the same thing.
- **Noted for a possible future revisit** (not implemented, not a user-facing setting): if Core 1 ever
  needs relief — after raising PCLK, or under a much heavier waypoint load — dropping the *GPS-driven*
  `RADAR_REFRESH` to 5Hz while leaving the compass at 10Hz would halve the render rate cheaply, since
  translation matters less than rotation. The compass rate is what makes the rotation feel right and
  should not be the thing lowered

**Fixed: committed `sdkconfig.cc-radar` had drifted from `sdkconfig.defaults`**

PlatformIO does not regenerate `sdkconfig.<env>` when `sdkconfig.defaults` changes — the first
240MHz build succeeded and still ran at 160MHz. Deleting the generated file and rebuilding revealed
three further settings that `sdkconfig.defaults` asked for and never got:

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` was **off** despite being requested — OTA rollback
  protection was configured but not armed. Now on; safe because `main.cpp:395` calls
  `esp_ota_mark_app_valid_cancel_rollback()`
- `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` still named the pre-OTA `partitions.csv`. Cosmetic only —
  PlatformIO's `board_build.partitions` governs the real table, and the built `partitions.bin` was
  verified to be the ota_0/ota_1 layout both before and after
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` existed **only** in the generated file, so regenerating
  reverted NimBLE allocations to IDF's INTERNAL default and would have pushed the beacon stack into
  the scarce internal SRAM. Now pinned in `sdkconfig.defaults`

**Radar frame 149ms → 94ms (~6.7 → ~10 fps): transpose tuning, then a zero-copy flush**

Steps 9b and 9 from the optimization backlog. Verified on hardware — no tearing, taps intact.

*Step 9b — transpose tuning.* `rotate90_tiled` gained `IRAM_ATTR` and an internal-SRAM scratch tile.
Tiling alone still left one of the two PSRAM streams strided: whichever loop is innermost gets
sequential access and the other hops by a 960-byte row stride. Transposing *via* a 2 KB SRAM tile
makes both PSRAM sides sequential — read a source row run, scatter into SRAM (uncached, free), emit
each destination row run as one `memcpy`. Rotation **64.1 → 55.7ms**.

Short of the ~40ms projected, and the reason was already sitting in the measurements: the flush
moved the same 460 KB PSRAM→PSRAM with an optimized `memcpy` in 34ms ≈ 27 MB/s, while the transpose
now runs at ~16.5 MB/s. Real headroom was ~1.6×, not the 1.6× *on top of* tiling that was assumed.

*Step 9 — `num_fbs = 2`, transpose straight into the back framebuffer.* The panel now allocates two
framebuffers. `rotate90_tiled` writes into the back one and hands that pointer to
`esp_lcd_panel_draw_bitmap`, which recognises its own framebuffer and swaps `cur_fb_index` instead
of copying (`esp_lcd_panel_rgb.c:614-624`). Flush **34.0 → 0.02ms** — deleted, not reduced.

Rotation *also* dropped **55.7 → 47.4ms**: the flush memcpy had been competing with the transpose
for PSRAM bandwidth and cache. That interaction reversed a step-9b decision — a 64-pixel tile beat
32 by 3.4ms while the flush existed, and by 0.5ms (noise) once it was gone, so the tile went back to
32 and kept 6 KB of SRAM.

- **Frame 149.6 → 94–101ms** depending on HUD content; rotation is now 50% of what remains
- **PSRAM −460 KB net**: +460 KB for the second framebuffer, −920 KB from no longer allocating the
  two rotation staging buffers at all
- **RAM +2,064 bytes** (the scratch tile), **Flash +872 bytes**
- `full_refresh = 1` is now load-bearing for the zero-copy path — a partial flush area would leave
  the rest of the alternate framebuffer holding a two-frames-old image. It moves in lockstep with
  the rotation mode in both init and the runtime `rot` switch, since LVGL rejects `full_refresh`
  together with `sw_rotate`
- Added an `on_frame_buf_complete` guard before the transpose: the driver latches
  `bb_fb_index = cur_fb_index` only at a frame boundary, so between a swap and that latch the back
  buffer is still being scanned out. At 94ms/frame vs a 26.6ms panel period it never blocks — it is
  there so step 10 (higher PCLK) cannot silently reintroduce tearing

**Radar frame 238ms → 149ms: dropped the canvas, then found two hidden full-screen repaints**

Two changes, one of which was found only because the other forced better instrumentation.

*Step 8 — drop the radar canvas (`816b421`).* The radar painted into a full-screen `lv_canvas`,
which LVGL treated as an image and blitted into the draw buffer on every refresh. Replaced with a
plain `lv_obj` that paints itself from an `LV_EVENT_DRAW_MAIN` handler, emitting geometry straight
into LVGL's draw context. Frame **238 → 210ms**, plus **460 KB of PSRAM freed**.

Far less than the ~104ms projected. The projection assumed the whole un-instrumented refresh
remainder (`refr − rot − flush`) was the canvas blit; it was not. The blit was worth ~22ms.

*Step 8b — `clip_corner` was defeating LVGL's cover-check (`44f6d0d`).* Bracketing the background
fill with a `DRAW_MAIN_BEGIN` timestamp split the remaining 82ms into `bg 23ms` + `non-radar 62ms`.
That 62ms was the screen background and the stage background being painted every frame — both
full-screen, both opaque, both the same green, both immediately covered by the radar.

Cause: `lv_obj_set_style_clip_corner(stage, true)`. LVGL answers `LV_EVENT_COVER_CHECK` with
`LV_COVER_RES_MASKED` for any object with `clip_corner` set, and `lv_refr_get_top_obj` treats
`MASKED` as *stop, do not descend into children*. The search for the topmost fully-covering object
bailed at the stage and never reached the radar, so LVGL drew from the screen down.

`clip_corner` also installs a radius mask that every child draw call blends through — which is what
made grid drawing 3× more expensive after step 8 moved painting inside the stage. One flag, both
symptoms. Frame **210 → 149ms**; grid **20–26 → 6–9ms**.

- **~0.8 fps → ~6.7 fps** across the full effort (frame ~499ms → 149ms)
- Nothing lost visually — the panel is physically round, so the clipped corners are not on the glass
- Timing semantics changed: painting now happens *inside* the LVGL refresh, so `paint` is a
  component of `refr`, not sequential with it. Frame = `label + refresh`. `perf`, the DEV tab and
  the on-screen HUD updated to match; `flush_us` added to attribute `esp_lcd_panel_draw_bitmap`
- Remaining: rotate 64ms / flush 34ms / bg 21.5ms / non-radar 16.4ms / paint 13ms
- Build: RAM 59.1% (193,520 B), Flash 79.4% (1,664,747 B)
- Files: `src/ui/ui_manager.cpp`, `src/ui/navigation.cpp`, `include/ui/navigation.h`,
  `include/ui/ui_manager.h`, `src/core/device_manager.cpp`, `src/utils/diagnostics.cpp`,
  `src/ui/dev_screen.cpp`, `docs/performance_optimization_backlog.md`

### Fixed

**Waypoint taps stopped opening the detail screen (regression from `816b421`)**

`lv_obj_create()` sets `LV_OBJ_FLAG_CLICKABLE` (`lv_obj.c:436`) where `lv_canvas_create()` does not.
Replacing the radar canvas with a plain object silently made the radar surface the hit-test winner
for every touch, so presses stopped at it instead of reaching the stage handler that calls
`handleTapAt()`. The radar rendered identically either way, so this was invisible until a waypoint
was tapped. Fixed by clearing the flag — the surface is for painting, input belongs to the stage
beneath it. Fixed in `44f6d0d`, verified on hardware.

**Brightness could be set to 0% with no way to recover**

`MIN_BRIGHTNESS_PERCENT = 5` was defined in `system_config.h` but referenced nowhere. The only
protection was the settings slider's range, which does not cover the NVS restore path at boot: a
stored raw level of 1–2 passes the `> 0` guard and divides down to 0%, leaving the panel dark with
the only control on a screen no longer visible. Moved the floor into `backlight::setPercent()` — the
single point every caller passes through — and switched standby to `backlight::off()`, which
bypasses it so deliberate full-off still works. Fixed in `aa66982`, verified on hardware.

**Radar rotation dropped to 1 Hz whenever GPS acquired a fix**

Heading rotation felt smooth while searching for satellites, then became visibly choppy the moment a
fix appeared. The renderer was not getting slower — the render *rate* dropped 5×.

`processUIUpdate()` handled `COMPASS_UPDATE` by skipping the redraw whenever GPS was valid, on the
stated reasoning that "`RADAR_REFRESH` is queued in the same burst". Both events are produced by the
System Task, but at different rates: the compass read is gated to 20ms so it fires every 200ms tick
(5 Hz), while the GPS read is gated by `GPS_UPDATE_INTERVAL_MS = 1000` (1 Hz). The bursts coincide 1
time in 5. The other 4 compass updates advanced `ui.current_heading` and drew nothing, so rotation
was pinned to the 1 Hz GPS rate and each frame showed a heading up to a second stale.

Fixed by coalescing instead of suppressing. All four render-triggering cases (`RADAR_REFRESH`,
`COMPASS_UPDATE`, `ZOOM_CHANGE`, `ZOOM_CHANGE_REVERSE`) now call `requestRadarRender()`, which only
sets a flag; the UI Task calls `flushRadarRender()` once after draining the queue batch, still inside
`display_mutex`, with a standby guard so a queued refresh cannot paint a screen that is off.

- Rotation now tracks the fastest producer: **1 Hz → 5 Hz with a fix**
- Renders per UI Task loop capped at **1** (was up to 4) — strictly fewer worst-case renders than
  before, and resolves §3.3 of the perf backlog, which warned of 4 × 149ms with the mutex held
- Verified on hardware with a 14-satellite fix: rotation smooth, button remains responsive
- This was §3.2 of the perf backlog, ranked step 11 behind four large pipeline rewrites. It needed
  none of them — the preceding `fill_bg` fix had already made frames cheap enough
- Build: RAM 59.0% (193,424 B), Flash 79.3% (1,662,635 B)
- Files: `src/utils/task_manager.cpp`, `docs/performance_optimization_backlog.md`, `ROADMAP.md`

**Radar frame time: canvas clear was 59% of every frame (205ms → 21ms)**

`updateRadarDisplay()` cleared the 480×480 radar canvas with `lv_canvas_fill_bg()`. For
`LV_IMG_CF_TRUE_COLOR` that LVGL function takes a per-pixel path — `lv_img_buf_set_px_color()` plus
`lv_img_buf_set_px_alpha()` for every pixel, i.e. **460,800 out-of-line calls per frame**, each doing a
colour-format switch and pointer arithmetic into PSRAM. Measured at 205ms, an effective 2.25 MB/s.

The radar canvas has no alpha channel, so `set_px_alpha` was a no-op on every one of those calls.
Replaced with `lv_color_fill()` over `dsc->data`, which is semantically identical for this format.

- `fill_bg` 205.0ms → 21.3ms (9.6×); paint stage 215.3ms → 32.2ms (6.7×)
- Frame time at unchanged rotation: ~499ms → 316ms
- Found by measurement, not inspection — two prior hypotheses (both blaming software rotation) were wrong
- Follow-up measured: software rotation costs ~153ms/frame; base refresh blit ~131ms
- Files: `src/ui/navigation.cpp`, `docs/performance_optimization_backlog.md`

### Added

**DEV Render Timing HUD**

On-screen render instrumentation for the performance work tracked in `docs/performance_optimization_backlog.md`. Splits a radar frame into its two measurable halves so the cost of software rotation can be attributed rather than guessed:

- `paint` — time inside `updateRadarDisplay()` (canvas painting), microsecond resolution
- `refr` — LVGL blit + 90° software rotate + flush dispatch, via `disp_drv.monitor_cb`
- `total` and `fps` — FPS measured from real panel flushes (`NavState::flush_count`) over a 1s window

Visible on the radar screen when dev mode is on; follows the same show/hide path as the DEV label (boot, `showHUD`/`hideHUD`, and runtime `DEV_MODE_CHANGE`).

- Positioned `LV_ALIGN_CENTER, 0, 120` — absolute corners are clipped by the round bezel
- Files: `include/ui/navigation.h`, `src/ui/navigation.cpp`, `src/core/device_manager.cpp`, `src/ui/ui_manager.cpp`, `include/ui/ui_manager.h`, `src/utils/task_manager.cpp`

**Build-time rotation toggle for A/B testing**

`-DRADAR_ROTATION_DEGREES=0` disables software rotation so the cost of `sw_rotate` can be measured directly. UI renders sideways — intended for measurement only.

- Files: `include/core/system_config.h`

### Changed

**Disabled LVGL built-in perf monitor** (`LV_USE_PERF_MONITOR 0`)

It was enabled but aligned to `LV_ALIGN_BOTTOM_RIGHT`, which sits behind the bezel on this round 480×480 panel — never visible, while still costing a label refresh every 300ms. Replaced by the DEV render timing HUD above.

- Build impact: flash 1,660,864 → 1,660,803 bytes (−61), RAM unchanged at 193,408 bytes
- Files: `include/ui/lv_conf.h`

---

## [Beta] - 2026-04-13

### Added

**OTA Firmware Update via Web Browser**

Full over-the-air firmware update system accessible from the GPX web portal at `/update`. The device writes the new binary directly to the inactive OTA partition, sets the boot partition, and reboots. A one-shot server guard (`ota_already_triggered`) prevents a browser retry or stale tab from re-flashing the device after a successful update. The OTA page matches the dark monospace aesthetic of the rest of the portal.

- Dual OTA partition table (`partitions_ota.csv`): 2×2MB app slots + 11.7MB FFat
- `esp_ota_mark_app_valid_cancel_rollback()` called on successful boot (rollback safety)
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` added to `sdkconfig.defaults`
- Warning message on upload page: display interference during flash is expected
- Files: `src/gpx/gpx_server.cpp`, `partitions/partitions_ota.csv`, `sdkconfig.defaults`

**CalVer Versioning + Per-Build Stamp**

Build version `vYY.MM.DD` written to `include/core/fw_version_gen.h` by `scripts/gen_version.py` (PlatformIO pre-build extra_script). A Unix build timestamp `FW_BUILD_TS` is also written on every build, ensuring `settings_manager.cpp` always recompiles and `FW_STAMP_VAL` is unique per build.

- Version displayed on loading screen (radar green, `LV_ALIGN_BOTTOM_MID`)
- `version` serial command added to diagnostics
- Files: `scripts/gen_version.py`, `include/core/fw_version_gen.h`, `src/utils/settings_manager.cpp`

**Radar-Mode Boot Guarantee After Any Firmware Flash**

First boot after any firmware update (USB or OTA) now always lands in radar mode, regardless of previous WiFi settings.

- Root cause: `FW_STAMP_VAL` was `fnv1a(FW_VERSION)` — same-day builds produced identical stamps, NVS WiFi flags survived any same-day flash
- Fix: `FW_STAMP_VAL = FW_BUILD_TS` (Unix timestamp, unique per build). Any mismatch → WiFi boot flags cleared → radar mode
- OTA handler calls `settings_manager::prepareForOTAReboot()`: writes `fw_stamp=0` + clears `wifi_ap_en`/`wifi_sta_en` in one NVS transaction — stamp=0 always mismatches any real `FW_BUILD_TS`

### Changed

**GPX Web Portal — Dark Monospace Theme**

Main GPX upload page (`/`) redesigned to match the OTA page aesthetic: `#1a1a1a` dark background, monospace font, `#00ff00` green accents, dashed drag-drop zone, dark file list. Removed purple gradient, emojis, and white card. All JavaScript functionality (drag-and-drop, file list, download, delete) unchanged.

---

## [Beta] - 2026-03-25

### Added

**Beacon Found Indicator**

A 40×40 LVGL canvas overlay (circle + star) is drawn at screen center when the beacon has been marked as found (NVS `bcn_found` flag set). The indicator renders in white on the dark theme and black on daylight mode, matches the existing DEV label position, and hides automatically when the HUD is hidden.

**Beacon 4-Zone Musical Tempo**

Sonar beep intervals restructured to four distinct zones: VERY_FAR 1500ms, FAR 750ms, MEDIUM 500ms, CLOSE 250ms. EMA smoothing (α=0.4), ±3 dBm hysteresis between zone transitions, and trend detection over 10 samples prevent zone flicker. Replaces the previous linear interval mapping that produced choppy audio at close range (FT-04 resolved).

**Fixed Waypoint UX Improvements**

Proximity star drawn on the waypoint dot itself at three sizes scaled to zone distance. Waypoint label renamed to "Fixed:" with position tuned for safe screen margin. Auto-unfix triggers when distance exceeds 1km. North indicator is hidden when north-up mode is active (not needed when north is always up).

### Changed

**Beacon Scan Interval**

BLE scan interval reduced from 1000ms to 500ms, improving RSSI update responsiveness when near the target beacon.

**Beacon Found = Silent**

`updateSonar()` returns immediately when `g_found == true`. Prevents a NVS-persisted found state (from a previous session) from triggering audio at startup before the user resets the found flag.

**DEV Label Position**

DEV mode overlay label moved to screen center to avoid collision with other HUD elements at screen edges.

### Fixed

**WiFi AP Chunked HTTP Responses**

GPX management web page responses now sent in 1KB chunks via `httpd_resp_send_chunk()`. Fixes EAGAIN errors on the ESP32 soft-AP TCP buffer that caused partial page loads or browser timeouts when serving the file management UI.

---

## [Unreleased] - 2026-03-20

### Changed

**SRAM Optimization — NimBLE Migration**

Replaced Bluedroid BLE stack with NimBLE (`h2zero/NimBLE-Arduino@^1.4.0`) to resolve critical SRAM exhaustion caused by the Bluedroid stack consuming ~65KB at runtime.

**Root cause**: After BLE init, only ~2–7KB SRAM remained — not enough for SD card DMA buffers (~4–16KB). Every SD log flush failed with `sdmmc_read_blocks failed (257)`. The same low-memory condition caused LVGL heap fragmentation stalls, making double-press detection unreliable.

**Result**: NimBLE uses ~25KB SRAM (vs ~65KB Bluedroid), freeing ~40KB.

**Symptoms fixed**:
- ✅ SD card write failures every 30s — eliminated
- ✅ Double-press detection restored (heap stalls gone)
- ✅ ~40KB SRAM headroom when BLE active (was ~2–7KB)

**API changes** in `beacon_proximity.cpp`:
- Single `#include <NimBLEDevice.h>` replaces three Bluedroid headers
- `BLEDevice::*` → `NimBLEDevice::*`
- `BLEAdvertisedDeviceCallbacks` → `NimBLEAdvertisedDeviceCallbacks`
- `onResult(BLEAdvertisedDevice)` → `onResult(NimBLEAdvertisedDevice*)`
- Scan completion: timer-based → `g_pScan->isScanning()` (more reliable)
- SRAM guard threshold lowered: 60KB → 30KB
- All business logic (EMA, zones, hysteresis, trend, sonar) unchanged

**Files**:
- `platformio.ini` — added `h2zero/NimBLE-Arduino@^1.4.0`
- `src/hardware/connectivity/beacon_proximity.cpp` — NimBLE API rewrite

**Build impact**: −8,184 bytes static RAM (RAM: 59.3% / 194,152 bytes)

---

**SRAM Optimization — System Logger Heap Allocation**

System logger `g_buffer` (8KB) was a static `.bss` allocation — always in SRAM, even when logging was disabled.

**Fix**:
- `g_buffer` converted from `static char[8192]` to heap-allocated `char*` in `init()`
- `init()` only called when `settings.logging_enabled == true`
- `close()` frees the buffer
- **Normal mode: 0 bytes used** by the logger (no buffer, no mutex, no SD overhead)
- DEV mode with logging on: 8KB allocated from heap on demand

**Files**:
- `src/utils/system_logger.cpp` — heap allocation/free
- `src/core/main.cpp` — init gated behind `settings.logging_enabled`

**Build impact**: −8,184 bytes static RAM (included in NimBLE total above)

---

**SRAM Optimization — NTP Sync Removal**

WiFi NTP was running a 10-second polling loop in `loop()` even when WiFi was fully disabled. RTC (PCF85063) and GPS time sync via `task_manager::queueGPSTimeSync` provide all needed accuracy.

**Fix**: NTP polling timer removed from `loop()`. `ntp_sync::init(false)` disables auto-sync at boot.

**Files**: `src/core/main.cpp`

---

**LVGL Full-Frame Buffers (480 lines)**

Upgraded LVGL draw buffers from 160 lines to 480 lines (full frame). Eliminates the visible top-to-bottom screen wipe artifact that appeared during transitions when LVGL software rotation is active (the rotation requires a temporary buffer that fits cleanly in the 64KB `LV_MEM_SIZE`).

**Details**:
- Full frame: 480 × 480 × 2 bytes × 2 buffers = **921 KB PSRAM**
- 1 flush per frame (was 3 at 160 lines)
- `BUFFER_LINES = 480` in `include/core/system_config.h`

**Files**: `include/core/system_config.h`

---

### Fixed

**DEV Mode No Longer Forces Logging On**

DEV mode was calling `settings_manager::saveLoggingEnabled(true)` during boot, overwriting the user's `logging: OFF` NVS setting on every restart. User had to re-disable logging after every reboot.

**Fix**: Removed auto-enable block. Logging state is fully user-controlled.

**Files**: `src/core/main.cpp`

---

**WiFi Boot Settings Not Applied to Scanner**

`scanner.cpp` had a hardcoded `wifi_enabled = true` that was never synced from NVS. WiFi scanning ran at boot even with WiFi disabled in Settings, producing spurious scan errors in serial output.

**Fix**: Added `scanner::setWiFiEnabled(settings.wifi_enabled)` at boot alongside `wifi_manager::setEnabled()`.

**Files**: `src/core/main.cpp`

---

**GPX Description Truncation**

Three internal `tmp[]` buffers in `gpx_loader.cpp` were 192–256 bytes — too small for geocaching long-form descriptions, causing mid-sentence cuts in the waypoint detail screen.

**Fix**:
- All three buffers increased to 512 bytes (matches the line read buffer size)
- Added "..." truncation at last sentence boundary when the 1024-byte description buffer fills

**Files**: `src/gpx/gpx_loader.cpp`

---

**Waypoint Detail Screen — HINT Section Unreachable**

Long descriptions pushed the HINT section below the reachable scroll area, making it inaccessible even by scrolling to the bottom.

**Fix**: HINT section placed before DESCRIPTION in the flex column layout.
- HINT shows a tap-to-reveal header, hidden text by default
- DESCRIPTION scrolls below
- User can always reach HINT at the top of the scroll view

**Files**: `src/ui/waypoint_screen.cpp`

---

### Added

**Beitian BH-880 GPS+Compass Module**

Replaced Quectel LC76G GPS module with Beitian BH-880 (B1301N GPS chip + QMC5883L compass). Drop-in replacement on same UART pins (GPIO43/44, 115200 baud).

**GPS improvements**:
- 10Hz update rate (was 1Hz)
- Multi-constellation: GPS, GLONASS, BDS, Galileo, IRNSS, SBAS, QZSS (120 channels)
- Sensitivity: −163 dBm tracking, −148 dBm cold start
- Cold start: 28s, hot start: 1s

**Critical power note**: Module requires 3.6–5.5V. The board 3.3V rail is insufficient for a GPS fix. Solution: power VCC directly from LiPo positive (3.7–4.2V). QMC5883L compass rated 2.16–3.6V — works fine on 3.3V.

**Files**:
- `include/hardware/sensors/gps_bh880.h` / `src/hardware/sensors/gps_bh880.cpp`

---

**QMC5883L Compass System**

Replaced IMU/Gyro heading fusion with the QMC5883L magnetometer built into the BH-880 module. Compass is the sole heading source — GPS heading fusion removed.

**Architecture**:
- System Task reads QMC5883L every ~1s → queues `COMPASS_UPDATE` → UI Task applies to `ui.current_heading`
- All waypoints, off-screen indicators, and north indicator rotate from `ui.current_heading`
- Reaction time: ~1s. Smooth for walking speed.

**Critical I2C constraint**: Compass cannot be read from the I2C Task. The CST820 touch driver calls `Wire.requestFrom()` directly, bypassing `i2c_mutex`. Reading compass from I2C Task causes immediate `Wire.cpp requestFrom Error -1`. Must use System Task (tolerates occasional errors). *(Accurate for the Arduino build this entry describes; the `Wire` bypass no longer exists after the ESP-IDF migration — see `docs/compass_i2c_constraint.md`.)*

**Reference**: `docs/compass_i2c_constraint.md`

**Files**:
- `include/hardware/sensors/compass_qmc5883l.h` / `src/hardware/sensors/compass_qmc5883l.cpp`
- `src/utils/task_manager.cpp` — System Task reads + `COMPASS_UPDATE` queue message

---

**WMM Magnetic Declination**

Auto-computes magnetic declination from GPS fix using a truncated WMM2020 spherical harmonic model (n=1..3, ±1° accuracy). Applied to every compass reading to produce true heading.

**Behavior**:
- Computed once per session at first valid GPS fix
- Persisted to NVS, reused on subsequent boots until a new fix updates it
- Sign convention: `true_heading += declination` (positive = East declination)
- Example: Los Angeles area ~12.25° East at (34.1°N, 118.1°W) in 2026.2

**Files**:
- `include/utils/wmm_declination.h` / `src/utils/wmm_declination.cpp`

---

**Build: NimBLE Library**

Added `h2zero/NimBLE-Arduino@^1.4.0` to `platformio.ini` lib_deps.

---

## [v0.14.0] - 2026-01-30

### Added

**Beacon Proximity System**

BLE-based item finder that turns the radar into a proximity detector. When zoomed to 50m, the device scans for a configured BLE beacon MAC address and provides real-time visual + audio feedback.

**Visual Arc Gauge**:
- Cyan arc drawn clockwise around the radar circle outer edge
- Fills from 0° (no signal, -90 dBm) to 355° (full circle, -45 dBm) based on EMA-smoothed RSSI
- 14px line width at 228px radius — clearly visible without obscuring radar content
- Minimum 10° arc when any signal detected (always visible when in range)

**Audio Sonar Beeping**:
- Buzzer pulses at 1800ms (far) → 900ms → 500ms → 200ms (< 1m) as signal strengthens
- Can be independently disabled in Settings > Sound > Beacon Sound toggle
- Non-blocking state machine — zero impact on UI responsiveness

**RSSI Processing**:
- EMA smoothing (α = 0.4) prevents visual/audio jitter from signal fluctuations
- Zone-based detection (OUT_OF_RANGE / FAR / MEDIUM / CLOSE) with ±3 dBm hysteresis
- Requires 2 consecutive readings to confirm zone change (prevents oscillation)
- Linear trend detection over last 10 samples: APPROACHING / DEPARTING / STABLE

**Integration**:
- **Zoom-gated**: Only activates at 50m zoom — stops automatically when zooming out
- **BLE scanning**: 1s scan every 2s (50% duty cycle), early-exit when target MAC found
- **15s timeout**: Beacon marked lost if not seen for 15 seconds

**Settings (NVS persistent)**:
- Target MAC address, measured power (dBm @ 1m), path loss exponent
- Separate toggle for sound vs visual
- Keys: `bcn_en`, `bcn_snd`, `bcn_mac`, `bcn_pwr`, `bcn_n`

**Build Impact**: ~3,500 bytes flash, ~2KB RAM

**Files**:
- `include/hardware/connectivity/beacon_proximity.h` - API and state structures
- `src/hardware/connectivity/beacon_proximity.cpp` - BLE scanning, EMA, zone logic
- `src/ui/navigation.cpp:390-420` - `drawBeaconProximityGauge()` arc drawing
- `src/utils/task_manager.cpp:79-94` - Zoom-gating activation logic
- `src/utils/diagnostics.cpp:1255-1403` - Serial diagnostic commands
- `src/ui/settings_screen.cpp:1241-1310` - Beacon Sound settings toggle
- `src/utils/settings_manager.cpp:656-705` - NVS persistence

**Serial Commands**: `beacon status`, `beacon scan`, `beacon test`, `beacon zone`, `beacon trend`, `beacon debug`, `beacon reset`, `beacon mac/power/n`

**Reference Documentation**: [`docs/beacon_proximity.md`](docs/beacon_proximity.md)

---

**Gyro Calibration System (Calib Tab)**

New dedicated Calibration tab in Settings for gyro-based heading fusion. Provides smooth heading tracking when GPS is unreliable (stationary or slow walking).

**Features:**
- **Calib Tab**: Always visible in Settings (between Sound and DEV)
- **Two UI States**: DISABLED / RUNNING (calibrated) or NEEDS CALIBRATION
- **50m Zoom Activation**: Gyro only runs at 50m zoom to save resources
- **Persistent Calibration**: Saved to NVS, survives reboots indefinitely
- **One-time Calibration**: Calibrate once, valid for months/years
- **UI Feedback**: Shows "CALIBRATING..." during calibration process

**User Workflow:**
1. Enable toggle in Settings > Calib
2. Press Calibrate button (keep device still for ~5 seconds)
3. Status changes to "RUNNING" with "Calibrated: YES (saved)"
4. Gyro auto-activates when zooming to 50m for precision navigation

**Key Files:**
- `src/ui/settings_screen.cpp:2181-2400` - Calib tab UI and status display
- `src/utils/task_manager.cpp:69-106` - Zoom-based gyro activation
- `src/navigation/imu_sampling.cpp` - 100Hz gyro sampling and calibration
- `src/utils/settings_manager.cpp:669-703` - NVS persistence

**Technical Details:**
- QMI8658 IMU at 100Hz sampling rate
- Calibration requires 500 samples (~5 seconds)
- Bias values stored in NVS (imu_cal, imu_bx, imu_by, imu_bz)
- Heap allocation for calibration buffer (avoids stack overflow)

**Build Impact**: +500 bytes flash (minimal)

---

### Fixed

**CRITICAL: UI_Task Freeze After Extended Runtime (Priority Alpha)**

Fixed a thread-safety bug that caused the UI_Task to freeze after 7+ hours of runtime. The freeze was caused by unsafe LVGL calls from button callbacks during standby enter/wake operations.

**Root Cause Analysis:**
- `enterStandby()` and `wakeFromStandby()` were called directly from button callbacks
- Button callbacks run during `button::update()` which is OUTSIDE the display_mutex
- Both functions made LVGL calls (lv_obj_create, lv_timer_create, etc.) without mutex protection
- `wakeFromStandby()` also called `lv_timer_handler()` directly - potentially recursive/concurrent
- Over time, this corrupted LVGL internal state, causing UI_Task to hang

**Solution:**
1. Added `ENTER_STANDBY` and `WAKE_STANDBY` to UIUpdateType enum
2. Button callbacks now queue standby operations instead of direct calls
3. Standby operations are processed inside display_mutex (thread-safe)
4. Removed dangerous `lv_timer_handler()` call from `wakeFromStandby()`

**Key Changes:**
- `include/utils/task_manager.h:96-97` - Added ENTER_STANDBY, WAKE_STANDBY enum values
- `src/core/device_manager.cpp:635-696` - Queue standby ops instead of direct calls
- `src/utils/task_manager.cpp:548-560` - Handle ENTER_STANDBY/WAKE_STANDBY in processUIUpdate()
- `src/utils/standby_manager.cpp:126` - Removed unsafe lv_timer_handler() call

**Symptoms Fixed:**
- UI_Task becomes UNRECOVERABLE after ~452 minutes (7.5 hours)
- Button presses stop working (zoom, settings, etc.)
- Position stops updating on radar display
- Recovery attempts (suspend/resume) fail

**Build Impact**: +352 bytes flash (minimal)

### Added

**Button Sound Diagnostic Feature (Priority 2.11 Phase 1)**

Implemented basic buzzer functionality for diagnosing UI freeze issues. When enabled, a short chirp plays on button press, helping determine if the button hardware works when the UI becomes unresponsive.

**New Files**:
- `include/hardware/buzzer.h` - Buzzer interface
- `src/hardware/buzzer.cpp` - Buzzer implementation using TCA9554 EXIO pin 7

**Settings Changes**:
- New "Sound" tab in Settings (between Display and DEV)
- "Button Sound" toggle (default: OFF)
- "Test Beep" button for testing buzzer

**Settings Storage**:
- New NVS key `btn_sound` for button sound preference
- New `button_sound_enabled` field in RadarSettings

**Key Changes**:
- `src/hardware/input/button.cpp:118-131` - Plays chirp on button press when enabled
- `src/core/device_manager.cpp:698-703` - Buzzer initialization after button init
- `src/core/device_manager.cpp:707` - Buzzer update for timing management
- `src/ui/settings_screen.cpp:1262-1370` - Sound tab implementation
- `include/ui/ui_manager.h:119` - Added settings_tab_sound

**Build Impact**: +1.5KB flash (minimal)

---

## [v0.13.0] - 2026-01-21

### Added

**5-Phase Stability Overhaul - Thread-Safe UI Architecture**

Complete system stability rewrite to fix crashes caused by thread-unsafe LVGL access from button callbacks. The system now uses queue-based UI updates, mutex protection, hardware watchdog, enhanced crash logging, and task health monitoring.

**Phase 1: Queue-Based UI Updates**
- Button callbacks now queue UI requests instead of direct LVGL calls
- New UIUpdateType enum values: `ZOOM_CHANGE`, `ZOOM_CHANGE_REVERSE`, `SETTINGS_SCREEN`
- `processUIUpdate()` handles all UI operations safely within UI Task context
- Eliminates race condition between button ISR and LVGL timer handler

**Implementation**:
```cpp
// BEFORE (unsafe - caused crashes):
ui.cycleZoom();
navigation::updateRadarDisplay();

// AFTER (safe - queued):
UIUpdate update;
update.type = UIUpdateType::ZOOM_CHANGE;
task_manager::queueUIUpdate(update);
```

**Phase 2: Mutex Protection**
- `display_mutex` wraps `lv_timer_handler()` and UI queue processing
- `ui_state_mutex` protects UIState field access
- Thread-safe accessor functions: `getCurrentZoomLevel()`, `cycleZoomForward()`, `cycleZoomBackward()`
- `withDisplayMutex()` helper for safe LVGL operations from any context

**Phase 3: ESP32 Task Watchdog (TWDT)**
- Hardware-level detection of hung tasks
- 30-second timeout with warning (no panic by default)
- All tasks subscribe and feed watchdog every loop iteration
- ESP-IDF version compatibility (4.x and 5.x API support)
- New files: `include/utils/watchdog.h`, `src/utils/watchdog.cpp`

**Phase 4: Enhanced Crash Logging**
- `CrashInfo` structure with RTC memory persistence (survives reboot)
- `captureState()` records heap, PSRAM, task loops, last operation
- `logBootReason()` logs ESP32 reset reason on every boot
- `SYSLOG_CHECKPOINT()` macro for strategic crash location tracking
- Boot reason detection: power-on, software reset, brownout, panic, etc.

**Phase 5: Task Health Monitoring**
- `TaskHealth` structure tracks per-task health metrics
- `last_loop_time_ms` recorded every task iteration
- 5-second unresponsive threshold triggers recovery attempt
- `attemptTaskRecovery()` suspends/resumes hung tasks (max 3 attempts)
- Recovery logging with task identification

**Build Impact**: +4,892 bytes flash (1,402,149 bytes total, 44.6%), +48 bytes RAM

**Files Modified**:
- `include/utils/task_manager.h` - UIUpdateType enum, TaskHealth struct
- `src/utils/task_manager.cpp` - Queue processing, mutex, watchdog, health monitoring
- `src/core/device_manager.cpp` - Button callback uses queue instead of direct LVGL
- `include/utils/watchdog.h` - NEW: ESP32 TWDT wrapper API
- `src/utils/watchdog.cpp` - NEW: TWDT implementation with version compatibility
- `include/utils/system_logger.h` - CrashInfo struct, checkpoint macro
- `src/utils/system_logger.cpp` - RTC crash state, boot reason logging
- `src/core/main.cpp` - Watchdog initialization, boot reason logging

**Stability Improvements**:
- ✅ Zero crashes from rapid button presses (tested 10+ presses in 2s)
- ✅ No race conditions between button and UI tasks
- ✅ Hardware watchdog catches hung tasks before system freezes
- ✅ Crash state captured for post-mortem debugging
- ✅ Automatic task recovery attempts before giving up

---

**DEV Mode Enhancements**

Improved developer experience when DEV mode is enabled in settings.

**Auto-Enable Logging**:
- System logging automatically enabled when DEV mode is active
- Triggered on boot if `dev_tab_visible = true`
- Triggered when `dev show` command is used
- Serial output: `[DEV] Developer mode active - logging enabled`

**Power Management Override**:
- All automatic power management DISABLED when DEV mode is on
- No automatic brightness changes
- No automatic GPS preset changes
- No automatic standby entry
- Gives developer full manual control for testing
- Serial output: `[POWER] DEV MODE ACTIVE - All automatic power management DISABLED`

**Build Impact**: +156 bytes flash

**Files Modified**:
- `src/utils/power_manager.cpp` - Skip automatic power management when DEV mode enabled
- `src/utils/diagnostics.cpp` - Auto-enable logging on `dev show` command
- `src/core/main.cpp` - Auto-enable logging at boot when DEV mode already on

---

**Daylight Mode - High Contrast Outdoor Display**

New display mode optimized for outdoor visibility with bright sunlight.

**Color Scheme**:
| Element | Normal Mode | Daylight Mode |
|---------|-------------|---------------|
| Background | Dark green (`#3A9949`) | Light green (`#E0FFE0`) |
| Grid | Black | Black |
| Waypoints | Yellow with glow | Dark navy blue (`#000080`) |
| Center triangle | Red | Dark red |
| Glow effect | Enabled (soft yellow) | Disabled (no glow) |

**Shadow Overlay Adjustment**:
- Normal mode: 30% opacity (was 50%)
- Daylight mode: 0% opacity (invisible)
- Depth effect preserved in normal mode, maximum visibility in daylight

**HUD Labels with Background Containers**:
All HUD labels now have rounded background boxes for improved legibility:
- **Battery**: Dark green background (`#00AA00`), white text, 8px rounded corners
- **GPS Status**: Dark green background (`#006600`), white text
- **DEV Indicator**: Dark orange background (`#CC5500`), white text
- Backgrounds darken further in daylight mode for maximum contrast

**Settings Integration**:
- Toggle: Settings → Display → Daylight Mode
- NVS persistence: Setting saved across reboots
- Real-time switching: No restart required
- Description: "Use bright background for outdoor visibility"

**User Experience**:
- **Before**: Screen nearly invisible in direct sunlight
- **After**: High contrast colors readable outdoors
- **Result**: Usable navigation in all lighting conditions

**Build Impact**: +860 bytes flash, +16 bytes RAM

**Files Modified**:
- `include/settings_manager.h:40` - Added `daylight_mode = false` to RadarSettings
- `src/utils/settings_manager.cpp` - NVS load/save for daylight mode
- `include/ui/ui_manager.h` - Added shadow_overlay and HUD background references
- `src/ui/ui_manager.cpp` - HUD label backgrounds, updateDaylightMode() function
- `src/ui/navigation.cpp` - ColorScheme struct, getColorScheme(), light green daylight bg
- `src/ui/settings_screen.cpp:1201-1243` - Daylight Mode toggle in Display tab

**Brightness Verification**:
- Confirmed PWM at maximum (255 on 8-bit scale)
- Hardware limitation: Backlight is already at 100%
- Daylight mode compensates with high-contrast colors instead

---

## [v0.12.0] - 2025-10-26

### Added

**Independent Zoom Level Display**
- Zoom level now displayed separately from GPS status (always visible)
- Position: Bottom-center, above GPS status label
- Format: `[5km]` (in brackets for clear distinction)
- Updates independently when user changes zoom (single/double click)
- Works even when GPS is searching or disconnected

**User Experience**:
- **Before**: Zoom level only shown with GPS fix: "GPS: Fixed (8 sats) [100m]"
- **After**: Always visible: `[100m]` above "GPS: Searching..." or "GPS: Fixed (8 sats)"
- **Result**: User always knows current zoom level, even without GPS

**Visual Layout** (bottom-center):
```
        [5km]           ← Zoom level (always visible)
  GPS: Searching...     ← GPS status (changes color)
```

**Build Impact**: +148 bytes flash (1,394,769 bytes total, 44.3%), +16 bytes RAM

**Files Modified**:
- `include/ui/ui_manager.h:108` - Added `zoom_label` to UIState
- `src/ui/ui_manager.cpp:204-210` - Created zoom label above GPS status
- `src/ui/navigation.cpp:553-577` - Update zoom label independently from GPS status

---

**5km Zoom Level - Intermediate Regional View**
- New zoom level between 10km and 1km: **5km radius** with 1.25km grid spacing (60px)
- Zoom sequence: 10km → 5km → 1km → 500m → 100m → 10m (6 levels total)
- Provides better granularity for regional navigation (between city-wide and neighborhood views)
- Grid spacing follows progressive pattern: **48px → 60px → 80px → 96px → 120px → 160px** (squares get LARGER as you zoom in, all distinct)
- **User Feedback**: Grid squares visually distinguish each zoom level - 5km has medium squares between 10km (smallest) and 1km (larger)

**Center-Aligned Grid System**
- **All zoom levels** now have horizontal and vertical lines passing through screen center
- Creates smooth, natural zoom transitions - center crosshair remains anchored
- Grid spacing calculated to divide evenly into 240px (half-screen radius)
- Mathematical precision: Each zoom has exact integer number of grids per half-screen

**Grid Configuration** (480px screen width, all divisible for center line):
- **10km**: 48px spacing (10 lines total) ✓ Center line at 240px
- **5km**: 60px spacing (8 lines total) ✓ Center line at 240px
- **1km**: 80px spacing (6 lines total) ✓ Center line at 240px
- **500m**: **96px** spacing (5 lines total) ✓ Center line at 240px - **Distinct from 1km**
- **100m**: 120px spacing (4 lines total) ✓ Center line at 240px
- **10m**: **160px** spacing (3 lines total) ✓ Center line at 240px - **Distinct from 100m**

**User Experience**:
- **Before**: Some zooms had center lines, others didn't (felt "off-center")
- **After**: Every zoom has perfect center crosshair (horizontal + vertical lines)
- **Result**: Buttery smooth zoom transitions, user position always visually centered

**Waypoint Filtering Rules (Confirmed)**:
- **On-screen**: Waypoints within zoom radius (5km) shown as yellow circles
- **Off-screen arrows**: Waypoints within 10× radius (5-50km) shown as orange triangles
- **Filtered out**: Waypoints beyond 50km not displayed at 5km zoom
- **Same rule applies** to all zoom levels: 10km shows 0-10km on-screen, 10-100km as arrows

**Off-Screen Indicator Enhancement**:
- **Base width doubled** horizontally for better visibility at screen edges
- Height unchanged (maintains pointing direction clarity)
- Triangle now wider and more prominent without obscuring direction

**User Experience**:
- **Single Click**: Zoom in (10km → 5km → 1km → 500m → 100m → 10m → loops back to 10km)
- **Double Click**: Zoom out (reverse order: 10m → 100m → 500m → 1km → 5km → 10km)
- **Result**: More intuitive zoom control, better regional navigation granularity, improved waypoint visibility

**Build Impact**: +148 bytes flash (1,394,769 bytes total, 44.3%), +16 bytes RAM

**Files Modified**:
- `include/ui/ui_manager.h:27-34` - Added ZOOM_5KM enum, renumbered subsequent levels
- `include/ui/ui_manager.h:80-87` - All 6 zoom levels recalculated for center-aligned grids
- `src/ui/navigation.cpp:557-563` - Added "5km" label in zoom display switch
- `src/ui/navigation.cpp:374-398` - Doubled base width of off-screen indicator triangles
- `src/core/device_manager.cpp:590-603` - Changed double-click from resetZoom() to cycleZoomReverse()

### Changed

**Button Zoom Controls - Bidirectional Zoom**
- **Double-click behavior changed**: Now zooms OUT instead of resetting to default (100m)
- Single click: Zoom IN (10km → 5km → 1km → 500m → 100m → 10m)
- Double click: Zoom OUT (10m → 100m → 500m → 1km → 5km → 10km)
- Removed "reset to default zoom" feature (no longer needed with bidirectional control)

**User Experience**:
- **Before**: Single click zooms in, double click resets to 100m (always interrupts workflow)
- **After**: Single click zooms in, double click zooms out (smooth bidirectional control)
- **Result**: Natural zoom control without workflow interruption

**Build Impact**: No size change (function call replacement only)

**Files Modified**:
- `src/core/device_manager.cpp:591` - Serial log: "zooming out" instead of "resetting zoom to default"
- `src/core/device_manager.cpp:600` - Call `ui.cycleZoomReverse()` instead of `ui.resetZoom()`

---

**Standby Mode - Low-Power Sleep Function**
- GPIO0 4-second press enters standby mode (display OFF, GPS ON, WiFi/AP OFF)
- Any GPIO0 press wakes from standby, returning to previous screen
- Power consumption reduced from ~520mA (active) to ~55mA (standby) = 89% reduction
- Battery life: 5.8 hours active → 54 hours standby (3000mAh battery)
- Standby screen shows: "STANDBY MODE", battery %, time, "Press button to wake"
- 3-second transition screen before display turns OFF
- Statistics tracking: total standby count, total time, last duration

**User Experience**:
- **Entering Standby**: Hold GPIO0 for 4 seconds (2s more after Settings opens)
- **Standby Screen**: Black screen with white text, shows for 3 seconds
- **Display OFF**: Backlight fades to 0%, GPS continues tracking in background
- **Waking**: Press GPIO0 once, display turns ON, returns to exact same screen (Settings or Radar)
- **Result**: Long-term field use without draining battery, GPS track never interrupted

**Technical Architecture - Overlay Approach**:
- **Problem Solved**: Original screen-switching approach caused LVGL object invalidation and NULL pointer crashes
- **Solution**: Full-screen overlay on top of current screen instead of loading new screen
- **Key Insight**: `lv_obj_create(current_screen)` creates child overlay, preserving parent screen objects
- **Wake Mechanism**: Simply delete overlay → underlying screen reappears (no navigation needed)
- **Advantage**: Wakes to exact same screen you left, no object corruption, simpler code

**Power Settings Applied in Standby**:
- Display backlight: 0% (PWM OFF)
- WiFi scanning: Disabled
- AP mode: Disabled
- GPS module: Remains ON (continuous tracking requirement)
- Task update rates: Unchanged (future optimization opportunity)

**Button State Machine Enhancement**:
- Added `EXTRA_LONG_PRESS` event type (4-second threshold)
- Dual-threshold detection: checks 4s first, then 2s (prevents Settings opening during standby entry)
- Button state includes `extra_long_press_triggered` flag

**Standby Manager Module** (`src/utils/standby_manager.cpp`, `include/utils/standby_manager.h`):
- `enterStandby()` - Creates overlay, saves state, starts 3s timer
- `wakeFromStandby()` - Restores power settings, removes overlay
- `isStandby()` - Query current state
- `getStats()` - Retrieve usage statistics
- `StandbyState` enum: ACTIVE, ENTERING, STANDBY, WAKING

**Implementation Details**:
```cpp
// Create overlay on current screen (not new screen!)
lv_obj_t* current_screen = lv_scr_act();
g_standby_screen = lv_obj_create(current_screen);  // Child of current screen
lv_obj_set_size(g_standby_screen, LV_HOR_RES, LV_VER_RES);
lv_obj_set_style_bg_color(g_standby_screen, lv_color_black(), 0);
lv_obj_move_foreground(g_standby_screen);  // Bring to front

// Wake: simply delete overlay
lv_obj_del(g_standby_screen);  // Current screen reappears
```

**Critical Bug Fixed During Development**:
- **Issue**: NULL pointer crash (LoadProhibited at 0x00000020) when waking from standby
- **Root Cause**: Original `lv_scr_load()` approach invalidated radar canvas objects
- **Solution**: Overlay approach eliminates screen switching entirely
- **Result**: Zero crashes, clean wake transition

**Build Impact**: +3,592 bytes flash (1,394,653 bytes total, 44.3%), +32 bytes RAM

**Files Added**:
- `include/utils/standby_manager.h` - Public API and types
- `src/utils/standby_manager.cpp` - Implementation (280 lines)

**Files Modified**:
- `include/hardware/input/button.h:14-20` - Added EXTRA_LONG_PRESS event
- `include/hardware/input/button.h:26` - Added extra_long_press_ms config
- `include/hardware/input/button.h:37` - Added extra_long_press_triggered flag
- `src/hardware/input/button.cpp:62-84` - Dual-threshold detection logic
- `src/core/device_manager.cpp:6` - Added standby_manager include
- `src/core/device_manager.cpp:412-420` - Added EXTRA_LONG_PRESS case, standby wake check
- `src/core/main.cpp:29` - Added standby_manager include
- `src/core/main.cpp:294-297` - Added standby manager initialization
- `src/ui/navigation.cpp:62-77` - Added radar canvas validation (defensive)
- `src/ui/navigation.cpp:527-545` - Enhanced updateRadarDisplay() validation

**Serial Commands**: None (future: `standby stats`, `standby enter`, `standby wake`)

**Reference Documentation**: [`docs/standby_mode.md`](docs/standby_mode.md) - Complete technical guide

---

## [v0.11.0] - 2025-10-21

### Changed

**Display Rotation System (Enclosure Design Adaptation)**
- Software rotation to compensate for 90° CCW physical display rotation in enclosure
- LVGL software rotation configured BEFORE driver registration (critical for RGB panels)
- User sees UI upright despite physical display orientation change
- Touch input automatically transformed to match rotated display

**User Experience**:
- **Before**: UI would appear sideways if display physically rotated in enclosure
- **After**: Software 90° CW rotation compensates, UI appears upright to user
- **Result**: Seamless adaptation to enclosure design changes without hardware modifications

**Technical Details**:
- **Rotation Method**: LVGL 8.x `disp_drv.sw_rotate = 1` + `disp_drv.rotated = LV_DISP_ROT_90`
- **Configuration**: `system_config::display::ROTATION_DEGREES = 90` constant
- **Critical Timing**: Rotation must be set BEFORE `lv_disp_drv_register()` call
- **RGB Panel Limitation**: Post-registration `lv_disp_set_rotation()` only transforms touch, not graphics
- **Touch Transform**: LVGL automatically adjusts touch coordinates for rotated display
- **Frame Buffer**: No changes required - software rotation handles pixel remapping

**Implementation Approach**:
```cpp
// CORRECT: Set rotation properties before registration
disp_drv.sw_rotate = 1;
disp_drv.rotated = LV_DISP_ROT_90;
lv_disp_t* disp = lv_disp_drv_register(&disp_drv);

// WRONG: Post-registration only rotates touch input
lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
lv_disp_set_rotation(disp, LV_DISP_ROT_90);  // Graphics stay unrotated!
```

**Build Impact**: No flash/RAM impact (compile-time constant, existing LVGL feature)

**Files Modified**:
- `include/core/system_config.h:36-37` - Added `ROTATION_DEGREES = 90` constant
- `src/core/device_manager.cpp:2` - Added `#include "system_config.h"`
- `src/core/device_manager.cpp:453-477` - Implemented pre-registration software rotation
- `CLAUDE.md:223-256` - Added Display Rotation documentation section

**Known Limitations**:
- Rotation is compile-time constant (requires rebuild to change)
- Only supports 90°/180°/270° rotations (LVGL limitation, not arbitrary angles)
- RGB panels require software rotation (hardware rotation not supported)

**Future Enhancements**:
- Runtime rotation selection via settings UI (if needed for different enclosure variants)
- NVS persistence of rotation preference

**Reference Documentation**:
- LVGL 8.x Display Rotation: https://docs.lvgl.io/8.3/porting/display.html#rotation
- ESP32 RGB Panel Characteristics: Requires pre-registration rotation configuration

### Improved

**Screen Tearing Reduction (Extra-Large LVGL Buffers)**
- Increased LVGL buffer size to 160 lines (maximum practical size)
- Minimizes "staircase" tearing artifacts during scrolling and screen transitions
- Reduces visible split/offset rendering where half of elements update before the other half

**User Experience**:
- **Before**: Visible diagonal "staircase" pattern during screen transitions (radar → settings)
- **Before**: Scrolling showed split buttons/elements (half moves first, other half follows)
- **After**: Significantly reduced tearing (3 flush operations instead of 10)
- **Result**: Much smoother display updates, though not completely tear-free

**Technical Details**:
- **Root Cause**: Asynchronous DMA transfers from PSRAM while display scans (no sync mechanism)
- **Attempted Solution**: Hardware bounce buffer (not supported in this ESP-IDF version)
- **Actual Solution**: Dramatically increased LVGL buffer size from 120 to 160 lines
- **Buffer Configuration**: 160 lines × 480 pixels × 2 bytes × 2 buffers = 295KB PSRAM
- **Flush Reduction**: 480÷160 = 3 flushes per frame (vs 4 with 120 lines, 10 with 50 lines)

**Why Larger Buffers Help**:
- Each flush operation creates a visible horizontal band during scrolling
- 10 bands (50-line buffers) were very noticeable
- 4 bands (120-line buffers) were less visible
- 3 bands (160-line buffers) are even less perceptible
- Partial refresh mode only updates changed areas for performance

**Limitations**:
- Tearing cannot be completely eliminated without hardware sync (VSYNC callback or bounce buffer)
- ESP-IDF version used doesn't support `bounce_buffer_size_px` feature
- 160 lines is practical maximum (larger buffers provide diminishing returns)

**Build Impact**: +73KB PSRAM usage (295KB total vs 221KB with 120 lines)

**Files Modified**:
- `include/core/system_config.h:34` - Changed `BUFFER_LINES` from 120 to 160
- `CHANGELOG.md:68-98` - Documented tearing reduction approach and limitations

---

**Smooth Scrolling in Settings UI (Display Buffer Optimization)**
- Dramatically increased LVGL buffer size from 50 to 120 lines
- Fixed visible horizontal band/block artifacts during Settings screen scrolling
- Reduced flush operations from 10 to 4 per frame (60% reduction)

**User Experience**:
- **Before**: Visible horizontal bands/blocks during scrolling (10 separate flush operations)
- **After**: Smooth, professional scrolling without tearing artifacts (only 4 flush operations)
- **Result**: Settings UI feels polished and responsive like commercial products

**Technical Details**:
- **Root Cause**: Small 50-line buffers required 10 flush operations per full screen update
- **Solution**: Increased buffer size to 120 lines (480×120 = 57,600 pixels per buffer)
- **Flush Reduction**: 480÷120 = 4 flushes per frame (vs 480÷50 = 10 flushes previously)
- **Rendering Behavior**: Fewer, larger chunks = less visible banding during scrolling
- **Refresh Mode**: Partial refresh (`full_refresh = 0`) for optimal performance

**Why Larger Buffers Work**:
- Each flush operation creates a visible horizontal band during motion
- 10 bands are very noticeable to the human eye during scrolling
- 4 bands are much less perceptible and create smoother visual experience
- Partial refresh mode still provides fast rendering by only updating changed areas

**Build Impact**: +129KB PSRAM usage (1.6% of 8MB available)

**Files Modified**:
- `include/core/system_config.h:34` - Changed `BUFFER_LINES = 50` to `BUFFER_LINES = 120`
- `src/core/device_manager.cpp:450-451` - Updated comment to reflect buffer optimization
- `CLAUDE.md:114-146` - Updated display optimization documentation

**Performance Impact**:
- CPU usage: No change (same partial refresh mode)
- Memory usage: +129KB PSRAM (221KB total for dual buffers)
- Flush operations: 60% reduction (4 vs 10 per frame)
- User perception: Significantly improved (smooth vs blocky scrolling)

---

## [v0.10.0] - 2025-10-20

### Added

**Heading-Up Navigation Mode (Priority 1 - Critical UX)**
- Radar rotates to match walking direction - user always moves "forward" on screen
- GPS heading extracted from NMEA RMC sentence (course and speed over ground)
- Automatic coordinate rotation system for waypoints and position indicators
- North indicator (red circle with white "N") shows true north relative to heading
- Stationary mode: maintains last heading for 10 seconds, then reverts to north-up
- Settings toggle: Switch between Heading-Up and North-Up modes (Settings > Display)
- NVS persistence: Navigation mode preference saved across reboots

**User Experience**:
- **Before**: North always at top, user must mentally rotate map when turning
- **After**: Walking direction always points up, map rotates automatically
- **Result**: Intuitive navigation matching Google Maps/Waze behavior

**Technical Details**:
- **GPS Heading**: Parsed from RMC fields 7-8 (speed knots, course degrees)
- **Heading Threshold**: 0.5 knots minimum speed for reliable heading
- **Rotation Algorithm**: `rotatePoint()` applies -heading rotation to all coordinates
- **North Indicator**: Calculated position at screen edge, rotates with heading
- **Coordinate Transform**: Applied in `latLonToScreen()` after Haversine calculation
- **Performance**: O(n) rotation per waypoint, <1ms for 50 waypoints @ 240MHz

**Build Impact**: +1,848 bytes flash (rotation system, north indicator, settings toggle)

**Files Modified**:
- `include/hardware/sensors/gps_lc76g.h:12-15` - Added course, speed, hasHeading to GPSData
- `src/hardware/sensors/gps_lc76g.cpp:37-90` - Parse RMC course/speed fields
- `include/ui/ui_manager.h:125-129` - Added heading state to UIState
- `src/ui/navigation.cpp:98-118` - Implemented rotatePoint() function
- `src/ui/navigation.cpp:158-161` - Apply rotation in latLonToScreen()
- `src/ui/navigation.cpp:248-286` - Added drawNorthIndicator() function
- `src/ui/navigation.cpp:528-541` - Heading update logic with stationary fallback
- `include/settings_manager.h:37` - Added heading_up_mode setting (default: true)
- `src/ui/settings_screen.cpp:1012-1051` - Navigation mode dropdown with NVS save
- `src/ui/ui_manager.cpp:54-59` - Load heading_up_mode from NVS on startup

**Memory Usage**:
- **Flash**: 1,342,041 bytes (42.7%) - was 1,340,193 bytes
- **RAM**: 100,772 bytes (30.8%) - unchanged

---

## [v0.9.0] - 2025-10-20

### Added

**WiFi/AP Auto-Disable in CRITICAL Power Mode (Priority 2.6 Phase 3)**
- Automatic WiFi scanning disable when battery ≤ 20%
- Automatic AP mode disable when battery ≤ 20%
- Prevents accidental battery drain from forgotten WiFi/AP connections
- User can manually re-enable WiFi/AP after automatic disable (override allowed)
- Serial logging for transparency: `[POWER] ✓ WiFi scanning disabled (Critical mode - auto power save)`
- Settings persistence: WiFi/AP state saved to NVS after auto-disable

**Technical Details**:
- Implementation: `src/utils/power_manager.cpp:262-279`
- Controlled by: `applyPowerMode(PowerMode::CRITICAL)` function
- Integration: Uses `scanner::setWiFiEnabled(false)` and `wifi_manager::setEnabled(false)`
- Settings fields: `settings.wifi_enabled`, `settings.wifi_ap_enabled`
- Power savings: ~80-120mA when WiFi disabled
- User override: Manual re-enable via Settings UI works immediately

**Build Impact**: +~200 bytes flash (WiFi/AP disable logic)

**Files Modified**:
- `src/utils/power_manager.cpp:1-7` - Added scanner.h include
- `src/utils/power_manager.cpp:262-279` - Implemented WiFi/AP auto-disable in CRITICAL mode

---

## [v0.8.0] - 2025-10-19

### Added

**Waypoint Glow Effect (Priority 3.8)**
- Static soft glow around all waypoint beacons for analog radar aesthetic
- Uses LVGL native shadow rendering (no sprite assets required)
- Configurable glow parameters via RadarConfig constants
- Soft yellow-white glow (color: 0xFFFF88) with 16% opacity
- 18-pixel glow radius with 2-pixel shadow spread
- Centered glow effect (no X/Y offset) for symmetric appearance
- Professional aviation-grade radar appearance

**Technical Details**:
- Implementation: LVGL shadow properties (shadow_width, shadow_color, shadow_opa, shadow_spread)
- Configuration: New RadarConfig constants in `ui_manager.h`
  - `WAYPOINT_GLOW_RADIUS = 18` (shadow width in pixels)
  - `WAYPOINT_GLOW_COLOR = 0xFFFF88` (soft yellow-white)
  - `WAYPOINT_GLOW_OPACITY = LV_OPA_40` (16% opacity)
  - `WAYPOINT_GLOW_SPREAD = 2` (shadow spread in pixels)
- Performance: <1ms additional render time for 8-10 on-screen waypoints
- Zero heap allocation: Pure LVGL styling

**Build Impact**: +~100 bytes flash (configuration constants + styling code)

**Files Modified**:
- `include/ui/ui_manager.h:68-72` - Added glow configuration constants
- `src/ui/navigation.cpp:319-325` - Applied glow effect to waypoint drawing descriptor

---

## [v0.7.1] - 2025-10-18

### Fixed

**GPIO0 Button Polling Fix (CRITICAL)**
- **Root Cause**: `button::update()` was NEVER called anywhere in the codebase
- **Symptom**: Button worked intermittently (timing-dependent)
- **Impact**: Settings menu inaccessible, zoom controls unreliable
- **Solution**: Added button polling to UI Task (`task_manager.cpp:90-92`)
- **Result**: Button now works 100% of the time
- **Build Impact**: +14,728 bytes flash

**Technical Details**:
- Button initialization was completing successfully
- Hardware interrupt fired but state machine never processed it
- Without polling loop, button was effectively non-functional
- Serial monitor presence changed timing (USB CDC affects interrupt latency)
- Fix: Poll button every 5ms in UI Task (highest priority, Core 1)

**Files Modified**:
- `src/utils/task_manager.cpp` - Added `device_manager::updateButton()` call

### Added

**Crash Logging System (ESP32 Core Dump)**
- ESP32 panic handler with 256KB flash partition
- Three new serial commands:
  - `crash dump` - View last crash information (PC address, crashed task, core dump version)
  - `crash info` - Show system capabilities and usage instructions
  - `crash clear` - Clear crash data (note: auto-overwrites on next panic)
- Automatic panic capture to flash (survives reboots)
- Program Counter (PC) tracking for crash location identification
- Crashed task identification (UI/I2C/Network/System)
- Core dump version and firmware SHA256 tracking
- Comprehensive troubleshooting workflow documentation

**System Capabilities**:
- ✅ Automatic panic capture to flash partition
- ✅ Survives reboot (persistent storage)
- ✅ Program counter (crash location)
- ✅ Crashed task identification
- ✅ Accessible via serial commands
- ⚠️ Limitations: PC requires firmware.elf for symbol lookup, single crash storage

**Configuration**:
- Enabled `CORE_DEBUG_LEVEL=3` in `platformio.ini`
- Uses existing 256KB coredump partition from `partitions.csv`

**Documentation**:
- Added "Crash Investigation Workflow" to `docs/troubleshooting.md`
- Pattern recognition guide (single crash vs reproducible bug)
- Common crash patterns (GPIO, battery, WiFi-related)
- Preventive monitoring checklist
- Advanced debugging with addr2line tool

**Build Impact**: +7,816 bytes flash

**Files Modified**:
- `platformio.ini` - Enabled core dump debug level
- `src/utils/diagnostics.cpp` - Added crash dump commands (~110 lines)
- `docs/troubleshooting.md` - Added crash investigation guide (~150 lines)

**WiFi/AP Mode Mutual Exclusion**
- Implemented user-requested behavior: WiFi and AP modes are now mutually exclusive
- Enabling WiFi → Automatically disables AP mode (stops GPX server, disconnects AP)
- Enabling AP → Automatically disables WiFi (disconnects from network)
- Both can be OFF for battery savings
- Clear serial logging with ⚠️ warnings for mode changes
- All state changes saved to NVS (persistent across reboots)
- UI toggles update automatically without user intervention

**User Experience**:
- Before: Both WiFi and AP could run simultaneously (confusing)
- After: Only one mode active at a time (clear, predictable)
- User controls: Enable desired mode, system handles coordination
- Serial feedback: Clear warnings when automatic changes occur

**Build Impact**: +1,100 bytes flash

**Files Modified**:
- `src/ui/settings_screen.cpp` - WiFi/AP coordination logic (~50 lines)

**Total Build Impact for v0.7.1**: +23,644 bytes flash (+1.8%), RAM unchanged

---

## [v0.7.0] - 2025-10-18

### Added

**GPX Waypoint Enhancements (Priority 2.8 - Quick Wins Phase 1)**
- Refresh Waypoints button in GPS settings tab
  - Location: After Factory Reset button
  - Action: Calls `gpx_loader::refreshGPXFiles()` to reload from SD
  - Button: "🔄 Refresh Waypoints" (blue, 200x40px)
  - Feedback: Serial log "Loaded X waypoints from SD card"
  - Updates radar display and waypoint count label
  - Implementation: `src/ui/settings_screen.cpp:1612-1632`

- Waypoint Count Indicator in GPS settings tab
  - Display: "Waypoints: 15/50" (current/max)
  - Color coding:
    - Green (0x00FF00): 0-30 waypoints
    - Yellow (0xFFFF00): 31-45 waypoints
    - Red (0xFF4444): 46-50 waypoints (approaching limit)
  - Dynamic updates via `updateWaypointCountLabel()` function
  - Implementation: `src/ui/settings_screen.cpp:1585-1610`

- Build impact: +752 bytes flash, no RAM change

### Improved

**Settings UI/UX Improvements (Priority 2.13)**
- Added 100px bottom padding to all settings tabs
  - GPS tab (line 628)
  - WiFi tab (line 774)
  - Display tab (line 844)
- Benefits:
  - Bottom elements can scroll to middle of screen
  - Improved touch accuracy for bottom buttons
  - Better readability of bottom text elements
  - Professional app-like scrolling experience
- Build impact: +16 bytes flash

**Waypoint Filtering System Documentation**
- Complete technical documentation in `docs/waypoint_filtering.md`
- Dual-strategy filtering system:
  - Distance-based filtering (10× zoom radius multiplier)
  - Sector-based clustering (maximum 8 off-screen indicators)
- Performance characteristics documented
- Algorithm references added to CLAUDE.md

---

## [v0.6.0] - 2025-10-17

### Added

**Loading Screen with LVGL Spinners (Priority 2.7)**
- Professional boot sequence with animated spinner
- Implementation details:
  - 75px spinner with ease-in-out animation
  - 2-second rotation period for smooth motion
  - Title: "GPS RADAR SYSTEM" (Iosevka 20pt)
  - Status: "Initializing..." (Iosevka 16pt)
  - Dark grey background (#262626)
  - 5-second display duration
  - Automatic transition to radar screen
- Benefits:
  - Professional startup experience
  - Clear system initialization indicator
  - Reduces user confusion during GPS lock
  - Smooth transition animations

**Key Files**:
- `src/ui/ui_manager.cpp` - Loading screen creation
- `src/core/main.cpp` - Boot sequence integration
- `include/core/system_config.h` - Configuration constants

**Actual Time**: 2 hours (as estimated)

---

## [v0.5.0] - 2025-10-15

### Added

**Battery Percentage Display on Radar Screen (Priority 1.6)**
- Always-visible battery percentage (top-right corner)
- Color-coded status indicators:
  - Green (0x00FF00): >70% battery
  - Yellow (0xFFFF00): 50-70% battery
  - Red (0xFF0000): <50% battery
- Auto-updates every 5 seconds via System Task
- Simple percentage-only format: "69%"
- Integrated with existing battery monitoring system

**Critical Implementation Details**:
- Position: `-150px from right, +20px from top`
  - Circular clipping requires aggressive inset
  - Initial `-50px` caused text cutoff and system crashes
- Short text format prevents cutoff on circular boundary
- Z-order: Created before shadow overlay for visibility
- Updates integrated into System Task (`task_manager.cpp`)

**Display Location**:
```
┌─────────────────────────────────┐
│                            69%  │ ← Top-right (-150px, +20px)
│                                 │
│         RADAR DISPLAY           │
│                                 │
│                                 │
│    GPS: Fixed (6 sats)          │ ← Bottom-center
└─────────────────────────────────┘
```

**Battery Monitoring vs Display**:
Two separate but connected systems:
1. **Monitoring System** (`battery.cpp`)
   - Collects voltage samples via GPIO4 ADC
   - Performs trend analysis (charging/discharging/stable)
   - Provides serial diagnostics
   - Controlled via `battery monitor on|off` command

2. **Display System** (`ui.battery_label`)
   - Visual percentage indicator on radar
   - Always visible and updating
   - Independent of serial monitoring setting

**Serial Commands**:
```
battery status         # Show voltage, percentage, state
battery monitor on     # Enable periodic serial logging (every 60s)
battery monitor off    # Disable periodic logging (UI still works)
battery voltage        # Show current voltage reading
battery history        # Show voltage trend history
```

**Key Files**:
- `include/ui/ui_manager.h:99` - Battery label field
- `src/ui/ui_manager.cpp:129-135` - Label creation with safe positioning
- `src/utils/task_manager.cpp:695-714` - Update logic with color coding
- `src/hardware/sensors/battery.cpp` - Battery monitoring system

**What We Skipped** (as requested):
- Charging icon/animation on display (use board CHG LED instead)
- Voltage display on screen (available via serial only)
- Complex battery state transitions on UI (serial only)

**Reference Documentation**:
- `docs/battery_monitoring.md` - Complete battery system guide
- `docs/battery_display_summary.md` - Implementation history

---

## [v0.4.0] - 2025-10-12

### Added

**GPS Settings UX Simplification (Priority 2.5)**

**Background**:
User testing identified two critical UX issues:
1. GNSS Systems Checkboxes (5 checkboxes) overwhelming for average users
2. Update Rate Manual Selection confusing and error-prone

**Solution: Smart Presets System**

**GNSS Systems → Preset Dropdown**:
- Replaced 5 checkboxes with intelligent preset system
- 5 options: Battery Saver, Balanced (default), Best Accuracy, Maximum, Custom...
- "Custom..." option opens modal with 5 checkboxes for advanced users
- Default: "Balanced" (GPS + GLONASS, ~55 satellites, global coverage)

**GNSS Preset Mappings**:

| Preset | Systems Enabled | Bitmask | Satellites | Use Case |
|--------|----------------|---------|------------|----------|
| Battery Saver | GPS only | `0x01` | ~31 | Longest battery life, acceptable accuracy |
| Balanced (Default) | GPS + GLONASS | `0x03` | ~55 | Best balance of accuracy/battery |
| Best Accuracy | GPS + GLONASS + Galileo | `0x07` | ~85 | High accuracy for navigation |
| Maximum | GPS + GLONASS + Galileo + BeiDou | `0x0F` | ~120 | Maximum accuracy, faster fix |
| Custom | User-defined | Variable | Variable | Advanced users only |

**Update Rate → Auto-Calculated**:
- Removed manual dropdown entirely
- Auto-calculate rate based on positioning mode:
  - Pedestrian → 1 Hz (1000 ms) - walking speed
  - Automotive → 5 Hz (200 ms) - driving speeds
  - Fitness → 2 Hz (500 ms) - running/cycling
  - Aviation → 10 Hz (100 ms) - high-speed flight
- Display: "Update Rate: 5 Hz (auto)" (read-only indicator)

**Implementation Complete**:
- ✅ Backend helper functions with comprehensive logging
- ✅ Help modal system (4 help icons)
- ✅ Phase 1-7 all completed and tested
- ✅ Comprehensive documentation (`docs/gps_settings_simplification.md`)

**Serial Logging**:
All GPS settings changes logged with consistent tags:
```
[GNSS_PRESET]   # GNSS preset selection and bitmask mapping
[GNSS_CONFIG]   # GNSS configuration breakdown
[AUTO_RATE]     # Auto update rate calculation
[GPS_SETTINGS]  # General GPS settings changes
[SETTINGS]      # NVS save/load operations
```

**Success Criteria Achieved**:
- ✅ Average users can configure GPS without technical knowledge
- ✅ Preset names are self-explanatory
- ✅ Update rate is automatically optimized
- ✅ Advanced users can still access full control via "Custom..."
- ✅ All settings persist across reboots

**External Zoom Button (Priority 1.1)**
- GPIO0 button used for zoom cycling
- Hardware button successfully implemented
- Touch screen freed for waypoint interaction
- Maintains 5-level zoom cycle behavior

**Settings Menu Trigger (Priority 1.2)**
- GPIO0 long-press detection (2-3 seconds)
- Enter/exit settings menu working
- Non-blocking button detection implemented
- Settings accessible via long-press

**NVS Storage System (Priority 2.1)**
- Non-Volatile Storage (NVS) for persistent settings
- User preferences stored across reboots
- Safe write operations with error handling
- Default values on first boot implemented

**Settings Menu UI (Priority 2.2)**
- Full-screen settings interface with tabbed navigation
- Touch-based navigation working
- Visual feedback for all controls
- Settings automatically saved (no explicit Save button)
- Red X button returns to radar screen

**Settings Categories**:
1. Display Settings (zoom, grid, brightness)
2. GPS Settings (update interval, logging, GNSS systems, positioning mode)
3. Waypoint Settings (persistent storage, max waypoints)
4. Advanced Settings (show coordinates, heading, speed)

---

## [v0.3.0] - 2025-10-12

### Added

**WiFi Web Portal for GPX Upload (Priority 3.1)**

**Architecture**: Station Mode + Web Portal
- User connects radar to home/office WiFi
- Web portal accessible at `http://radar.local` or device IP address
- No mode switching needed - works on existing WiFi network

**Current Implementation**:
- ✅ WiFi Manager - Full STA mode with credential storage
- ✅ GPX Server - Complete web server with upload UI (`gpx_server.cpp`)
- ✅ Beautiful drag-and-drop interface (HTML/CSS/JS embedded)
- ✅ RESTful API - Upload, list, delete endpoints
- ✅ mDNS support - `http://radar.local` automatic discovery
- ✅ Integration - GPX server integrated in main.cpp loop (auto-starts when WiFi connects)
- ✅ Auto-loading - GPX files auto-loaded from SD card on boot (`gpx_loader.cpp`)
- ✅ Web Portal UI - URL displayed in WiFi settings tab

**Known Issues**:
- ⚠️ Web portal only accessible in AP mode
- ⚠️ mDNS not working in STA mode
- ✅ Workaround: Use IP address displayed in WiFi settings tab

**Web Portal Features**:
- 📂 Drag-and-drop GPX file upload
- 📋 View uploaded GPX files with delete option
- 🎨 Beautiful responsive UI (purple gradient design)
- 🔄 Real-time upload progress and status messages
- 🌐 mDNS discovery - `http://radar.local`
- 📍 Auto-creates `/gpx/` folder on SD card

**Web Portal Endpoints**:
```
GET  /              # Upload interface (HTML page)
POST /upload        # File upload handler
GET  /list          # List GPX files (JSON)
DELETE /delete/:filename  # Delete GPX file
```

**User Workflow**:
1. Long-press GPIO0 → Settings screen
2. Connect to WiFi network (via WiFi UI)
3. Settings displays: "Web Portal: http://192.168.1.100"
4. Open browser on phone/laptop → Drag-and-drop GPX files
5. Waypoints automatically appear on radar

---

## [v0.2.0] - 2025-10-07

### Added

**Multi-Level Zoom System (Priority 1.3)**
- 5 zoom levels with progressive grid sizing
- Touch-to-zoom interface (tap canvas to cycle zoom)
- Dynamic meters-per-pixel calculation
- Grid spacing adapts to zoom level (48px → 140px squares)
- Zoom level displayed in GPS status text

**Zoom Levels**:

| Level | Radius | Grid Spacing | Grid Size (pixels) | Use Case |
|-------|--------|--------------|-------------------|----------|
| 10km | 10000m | 2000m | ~48px | Long-range navigation |
| 1km | 1000m | 300m | ~72px | Local area |
| 500m | 500m | 200m | ~96px | Neighborhood |
| 100m (default) | 100m | 50m | ~120px | Street level |
| 10m | 10m | 5.83m | ~140px | Precision mode |

**User Interaction**:
- Tap radar canvas to cycle zoom: 10km → 1km → 500m → 100m → 10m → (loop)
- Current zoom level shown in status text: `GPS: Fixed (15 sats) [100m]`

**Key Files**:
- `include/ui/ui_manager.h` - ZoomLevel enum, ZoomConfig struct
- `src/ui/ui_manager.cpp` - Touch-to-zoom event handler
- `src/ui/navigation.cpp` - Zoom-aware coordinate conversion

**Edge-Aligned Grid System (Priority 1.4)**
- Perfect edge alignment at all zoom levels
- Grid lines always reach screen edges (0 and 479 pixels)
- No visual gaps or offsets
- Dynamic grid spacing based on zoom level

**Algorithm**:
```cpp
// Draw vertical lines from x=0 with grid_spacing_pixels intervals
for (int x = 0; x < screen_size; x += grid_spacing_pixels) {
    draw_line(x, 0, x, screen_size-1);
}
// Always draw right edge line at x=479
if ((screen_size - 1) % grid_spacing_pixels != 0) {
    draw_line(screen_size-1, 0, screen_size-1, screen_size-1);
}
```

**Key Files**:
- `src/ui/navigation.cpp:drawRadarGrid()` - Grid drawing implementation

**Visual Polish and UI Refinements (Priority 1.5)**
- ✅ Fixed triangle color (black → red #D43701)
- ✅ Geometric centering of center triangle (using centroid offset)
- ✅ Changed waypoint markers to circles (25x25px yellow)
- ✅ Removed "+ Waypoint" button (clean full-screen radar)
- ✅ Repositioned GPS status text higher (y=-40 instead of y=-10)
- ✅ Reduced GPS serial logging spam (every 10 seconds instead of every second)
- ✅ Removed "[RADAR] Update display" debug spam
- ✅ Fixed canvas positioning (explicit 0,0 with no padding)

**GPS Status Label**:
- Position: `ALIGN_BOTTOM_MID, y_offset=-40`
- Font: `lv_font_montserrat_14`
- Colors:
  - Searching: Yellow (#FFFF00)
  - Fixed: Green (#00FF00)
- Format: `"GPS: Fixed (15 sats) [100m]"`

**GPS Integration (Priority 1.2)**
- LC76G GPS module integration (UART on GPIO43/44, 115200 baud)
- NMEA sentence parsing (GGA, RMC)
- Real-time position accuracy: ±1-2 meters (15 satellites, HDOP=1.0)
- Visual GPS status indicator:
  - Yellow text: "GPS: Searching..." (no fix)
  - Green text: "GPS: Fixed (X sats)" (locked)
- Automatic center reference update (user position becomes center)

**Performance**:
- Position update rate: 1Hz (every second)
- Serial logging: Every 10 seconds (reduced spam)
- Typical accuracy: ±0.78m latitude, ±0.46m longitude

**GPS Data Structure**:
```cpp
struct GPSData {
    double lat, lon;      // Decimal degrees
    float alt;            // Altitude in meters
    float hdop;           // Horizontal dilution of precision
    int sats;             // Number of satellites
    bool valid;           // Fix status
};
```

**Key Files**:
- `src/gps/gps_lc76g.cpp` - GPS driver and NMEA parsing
- `src/utils/task_manager.cpp` - GPS task and serial logging
- `include/device_manager.h` - GPS data structures

---

## [v0.1.0] - 2025-10-07

### Initial Release

**Core Radar Display System (Priority 1.0)**
- Full-screen circular 480x480 radar display (green background)
- Edge-aligned black grid system (2px lines)
- Red equilateral triangle in center (44x44px, geometrically centered)
- Yellow circular waypoint beacons (25x25px)
- User-centered navigation (center triangle represents user position)
- Waypoints move relative to user as GPS position updates

**Key Files**:
- `src/ui/ui_manager.cpp` - Radar screen creation and canvas setup
- `src/ui/navigation.cpp` - Drawing functions and coordinate conversion
- `include/ui/ui_manager.h` - Radar configuration constants

**Technical Implementation**:
```cpp
// Radar visual elements
CENTER_TRIANGLE_SIDE = 44px     // Equilateral triangle sides
CENTER_TRIANGLE_HEIGHT = 38px   // Triangle height
WAYPOINT_SIZE = 25px            // Yellow beacon circles
GRID_LINE_WIDTH = 2px           // Black grid lines
```

**Radar Display Features**:
- 480×480 pixel circular display
- Green background (#00AA00)
- Black grid overlay (2px lines)
- Red center triangle (user position)
- Yellow waypoint markers
- Real-time GPS coordinate tracking
- Serial debug output (115200 baud)

---

## Development Notes

### Documentation Updates
- Complete battery monitoring guide in `docs/battery_monitoring.md`
- Battery display implementation summary in `docs/battery_display_summary.md`
- GPS settings simplification guide in `docs/gps_settings_simplification.md`
- Waypoint filtering technical deep-dive in `docs/waypoint_filtering.md`
- Custom fonts documentation in `docs/custom_fonts.md`
- WiFi implementation guide in `docs/wifi_implementation_guide.md`

### Build System
- PlatformIO project with ESP32-S3 support
- 16MB Flash, 8MB PSRAM
- Custom partition table for app and filesystem
- LVGL 8.3.11 graphics library
- Arduino framework

### Hardware Requirements
- Waveshare ESP32-S3-Touch-LCD-2.1 board
- Beitian BH-880 GPS + Compass module (GPIO 43/44 UART, GPIO 15/7 I2C)
- 3.7V LiPo battery
- MT3608 boost converter (3.3V → 5V for GPS module)

---

**Project Repository**: https://github.com/alvroga/db-radar
**License**: CC BY-NC-SA 4.0 — Alvaro Robles

**Last Updated**: 2026-05-09
