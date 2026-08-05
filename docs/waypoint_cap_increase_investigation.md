# Waypoint Cap Increase — Investigation

> **Superseded 2026-08-05.** The equirectangular rewrite this doc recommended is built, and
> `MAX_WAYPOINTS` is raised — to **500**, matching this doc's primary analysis target (not the 700
> the SRAM budget could otherwise have afforded; that tradeoff, and the actual decision, are recorded
> in [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md)). Kept below as historical context for
> the cost analysis; the "Recommendation" and "Open question" sections at the bottom are resolved, not
> current.

**Status**: Investigation only — no code changed. Written to answer "how do we raise
`MAX_WAYPOINTS` past 50 without breaking anything" before touching any code, per request.
**Date**: 2026-08-04

## The question

`RadarConfig::MAX_WAYPOINTS = 50` (`include/ui/ui_manager.h:72`) is a *load-time* cap — `gpx_loader`
stops parsing a GPX file once 50 waypoints are loaded and silently truncates the rest
(`gpx_loader.cpp:171,378-382`). A geocaching.com pocket query routinely has hundreds of caches, so
most of a real file never loads today.

The ask: can we store many more waypoints (the user's example: 500, "spread around the world") while
still only ever *drawing* a bounded, uncluttered set near the user — i.e. decouple "how many can be
loaded" from "how many are visible at once"?

**Short answer: yes, and the decoupling already exists.** `drawWaypoints()` culls by distance before
drawing (`navigation.cpp:689`) and renders only the fixed waypoint when one is selected
(`navigation.cpp:669`), so what appears on screen is already bounded by zoom-relative distance and
8-way sector clustering, independent of how many are loaded. Raising the cap doesn't change that
logic at all — it only changes how many candidates the filter runs over. The real question this doc
answers is what raising that candidate count costs, and where.

## What actually scales with the cap

### 1. SRAM — measured, not estimated

`sizeof(Waypoint)` was measured directly with the project's own cross-compiler (not assumed), by
compiling the current struct definition (`include/ui/ui_manager.h:32-41`) standalone with
`xtensa-esp32s3-elf-g++` and reading the object file's section sizes:

```
sizeof(Waypoint)       = 144 bytes   (0x90)
sizeof(WaypointDetail) = 1280 bytes  (0x500)  — desc[1024] + hint[256], PSRAM-resident
sizeof(void*)          = 4 bytes     — Xtensa is 32-bit; this project's own earlier ~136B
                                        estimate in memory/sram_budget.md assumed 8-byte
                                        pointers and was slightly off. 144B is exact.
```

Current static SRAM, read directly off the current build's ELF via `readelf -S`
(`.pio/build/cc-radar/firmware.elf` — matches the current working tree; g_ui_state's size in this
build is consistent with the desc/hint-in-PSRAM struct, not the pre-migration one):

```
.dram0.data  30,776 B   (PROGBITS — costs flash too, copied to RAM at boot)
.dram0.bss  102,280 B   (NOBITS — zero-init, no flash cost)
-----------------------
Static total 133,056 B  = 40.6% of the 327,680 B budget this project targets
```

This matches `memory/sram_budget.md`'s reported 132,384 B / 40.4% closely enough to trust both.

**Cost per waypoint added**: 144 B SRAM (the `Waypoint` array entry) + 1,280 B PSRAM (the
`WaypointDetail` block, allocated once via `heap_caps_calloc(MAX_WAYPOINTS, ...)` in
`ui_manager.cpp:44`).

**At a cap of 500** (+450 over today):

| | Delta | New total | New % of 327,680 B budget |
|---|---|---|---|
| SRAM | +450 × 144 B = +64,800 B (63.3 KB) | 197,856 B | **60.4%** |
| PSRAM | +450 × 1,280 B = +576,000 B (562.5 KB) | ~626.5 KB used | trivial — 8 MB total, currently 85%+ free per CLAUDE.md |

**60.4% is not a new number for this project.** Before the desc/hint→PSRAM migration
(ADR-0001), static SRAM sat at 194,152 B / 59.3% *with the old, much larger* `Waypoint` struct
holding only 50 entries — and that build ran in the field with no reported SRAM-pressure symptoms
(the symptoms that *were* reported and fixed, FT-01 double-tap and FT-04 buzzer stutter, were traced
to the Bluedroid→NimBLE BLE stack difference, not this struct). Raising the cap to 500 essentially
spends the ~64 KB the PSRAM migration freed to buy back 450 more waypoint slots — it doesn't push the
project into new territory, it returns to a slightly-higher version of where it already stood, with a
different (better) allocation of what's in that budget.

**PSRAM has no story here worth spending more words on** — 562.5 KB against a chip with 8 MB, already
carrying ~1.8 MB of framebuffers, is noise.

**What else might want this headroom instead?** Audited every ROADMAP.md "Planned" and "Known Issues"
entry for a competing SRAM claim:
- Beacon Direction Finding (unblocked, not yet built) — its sample buffer is a small histogram (RSSI +
  heading per 30° bin during a rotation), on the order of a few hundred bytes, not KB. Not a
  meaningful claim on this budget.
- Compass Calibration Level 4 (τ-per-zoom smoothing) — a handful of float constants, negligible.
- Everything else on the roadmap (FT-03 zoom progression, CRT theme, FT-06/FT-07 I2C work) is
  config-constant or logic-only, no new persistent SRAM.

**Nothing currently on the roadmap wants this SRAM.** The one standing reason *not* to spend all of it
is the project's own rule of thumb (`memory/sram_budget.md`: *"if total approaches 80% utilization,
revisit"*) — general safety margin, not a specific feature. With NimBLE's ~25 KB dynamic-only cost
(active solely at 50 m zoom) layered on top of the 500-cap static total: 197,856 + 25,600 = 223,456 B
= **68.2%**, leaving ~104 KB (31.8%) free — comfortably under that 80% line, and close to (slightly
better than) the pre-migration operating point the project already ran on. This is a static-link
calculation, not a live heap reading — worth confirming once with `memory stats` on hardware at the
new cap before shipping, the same way any SRAM-affecting change should be, but there's no reason from
this analysis to expect a surprise.

### 2. Per-frame render cost — this is the part that can actually break something

`drawWaypoints()` (`navigation.cpp:612-`) loops `for (int i = 0; i < ui.waypoint_count; i++)` —
**every loaded waypoint, every frame** — and, for each one not skipped by the fixed-waypoint
short-circuit (`navigation.cpp:669`, which reduces the whole loop to O(1) whenever a waypoint is
tapped/fixed), runs a full double-precision Haversine + bearing calculation
(`navigation.cpp:673-696`) **before** the distance filter (`navigation.cpp:689`) decides whether the
waypoint is even close enough to draw. Counted directly from the code, that's per waypoint:

- `cos(lat2)`, `sin(lat2)` — 2 calls (lat1's are hoisted once per frame, `navigation.cpp:656-658`)
- `sin(dLat/2)`, `sin(dLon/2)` — 2 calls
- `sqrt(a)`, `sqrt(1-a)` — 2 calls
- `atan2(...)` for the Haversine central angle — 1 call
- `sin(dLon)`, `cos(dLon)` for the bearing — 2 calls
- `atan2(...)` for the bearing — 1 call

**10 double-precision transcendental calls per waypoint**, unconditionally, regardless of whether
the waypoint ends up on-screen, off-screen-but-indicated, or filtered out entirely. The ESP32-S3's
FPU is single-precision only (stated in CLAUDE.md and ROADMAP.md's existing note on this exact cap),
so every one of those runs through newlib's soft-double math — meaningfully slower than the
equivalent `float` call would be.

**This is exactly the cost ROADMAP.md's "Waypoint Memory Optimization" entry already flags** — "what
scales with the cap is the per-waypoint Haversine loop... read `wpt_us` off the `perf` HUD before
raising the cap" — and it is the one part of this change I can't fully resolve without hardware.
Here's what's known and what isn't:

**Known** (from `docs/performance_optimization_backlog.md`, most recent zero-copy-architecture
measurement, 240 MHz): the whole "radar paint" stage — grid + waypoints + triangle/N/gauge combined —
costs **9.3 ms** out of an ~85-94 ms frame, measured with only **2-3 real-world waypoints loaded**.
CLAUDE.md's own Render Pipeline section notes this stage "did *not* move" between 160→240 MHz (1.01×
scaling), meaning at 2-3 waypoints the cost is dominated by fixed per-shape draw-buffer writes, not by
the trig math — which tells us nothing about the *marginal* cost per additional waypoint at scale.

**Not known**: nobody has loaded 200+ waypoints and read `wpt_us`. There is no isolated measurement of
the Haversine loop's per-waypoint cost at this project's own naming convention would call a "residual"
— and this project has three documented cases
(`memory/feedback_residual_attribution.md`) where guessing at an un-instrumented number instead of
measuring it turned out wrong. I'm not going to add a fourth. A rough bound: even if soft-double
`sin`/`cos`/`atan2`/`sqrt` cost only ~5-10 µs each on this core (a plausible but unverified figure for
Xtensa LX7 soft-float), 10 calls × 500 waypoints × ~7 µs ≈ **35 ms** — on the same order as the entire
current frame budget. That is a real risk, not a hypothetical one, and it's the one part of this plan
that must be field-verified before shipping any specific cap number.

### 3. The fix that removes the risk instead of just measuring it

ROADMAP.md/backlog §3.6 already proposes exactly the fix this needs, independent of the cap increase
(it's filed as "small, safe hygiene"): at radar scale (≤ a few km) the equirectangular approximation
is accurate to well under a pixel and replaces almost the entire per-waypoint cost:

```
dx_meters = R · Δlon · cos(lat_ref)     // cos(lat_ref) already hoisted once per frame as cos_lat1
dy_meters = R · Δlat
```

That's **2 multiplies** per waypoint (Δlon and Δlat are single subtractions already computed today),
replacing the Haversine block entirely. What's still needed downstream, and how cheap it can be made:

- **Distance** (needed for the filter at `navigation.cpp:689` and the proximity-star logic at
  `navigation.cpp:729`) becomes `sqrtf(dx*dx + dy*dy)` — one **single-precision** sqrt, not a double
  `sqrt`+`atan2` pair.
- **Bearing** (needed *only* for off-screen sector clustering, `navigation.cpp:766-`) becomes
  `atan2f(dx, -dy)` — single-precision. At radar scale the difference from the current
  spherical-bearing formula is sub-degree, same accuracy argument the backlog already makes for
  distance. Note the current code computes bearing **unconditionally**, even for on-screen waypoints
  that never use it (`navigation.cpp:694-696` runs before the on/off-screen check at
  `navigation.cpp:717`) — moving it inside the off-screen branch is a second, independent saving not
  currently called out in the backlog, worth doing at the same time.

Net: **10 double transcendental calls → 2 multiplies + 1 `sqrtf` + (conditionally) 1 `atan2f`** per
waypoint. That's not a marginal win, it's close to an order of magnitude, and it's very likely enough
to make even a 10× waypoint-count increase cost-neutral against today's 50-waypoint baseline — but
"very likely" is still a claim to verify with `wpt_us`, not assert.

### 4. Everything else that touches `MAX_WAYPOINTS` — audited, no other risk found

Every call site of `MAX_WAYPOINTS` in the codebase, and what raising it does to each:

| File:line | What it does | Risk at cap=500 |
|---|---|---|
| `ui_manager.cpp:44` | `heap_caps_calloc(MAX_WAYPOINTS, sizeof(WaypointDetail), MALLOC_CAP_SPIRAM)` | None — PSRAM, see above |
| `gpx_loader.cpp:171` | Parse loop bound | None functionally; boot-time cost, see §5 below |
| `gpx_loader.cpp:378-382` | Truncation flag + warning | Still correct at any cap — just fires later |
| `gpx_loader.cpp:406` | `clearWaypoints()` reset loop | O(cap), trivial (simple field resets, no I/O) |
| `settings_screen.cpp:2019` | `"Waypoints: %d/%d"` **summary label only** | **Checked specifically because a per-waypoint list widget would risk the 64 KB LVGL pool** (`LV_MEM_SIZE`, `lv_conf.h:8` — sized for "~60 widgets" per its own comment). It is not a list — one label, one string. No LVGL pool risk. |
| `gpx_server.cpp:1061` | `{"count":N,"max":M}` JSON, one `snprintf` into a 48-byte buffer | None |

**One real bug found, unrelated to render/memory**: `updateWaypointCountLabel()`
(`settings_screen.cpp:2028-2034`) color-codes the count label with hardcoded absolute thresholds —
green ≤30, yellow ≤45, red >45 — tuned for a cap of 50 (60%/90%). Raising the cap without touching
this shows **green at 490/500 waypoints loaded**, which is exactly backwards. Needs to become
proportional to `MAX_WAYPOINTS` (e.g. ≤60%/≤90% of cap) as part of the same change.

No fixed-size stack arrays sized to `MAX_WAYPOINTS` were found anywhere outside the two documented
PSRAM/SRAM arrays (`Waypoint waypoints[MAX_WAYPOINTS]` in `UIState`, the `WaypointDetail` PSRAM block)
— nothing else silently grows or risks a stack overflow.

### 5. Boot-time parse cost — lower severity, still unmeasured

`gpx_loader::parseGPXFile()` reads a file **character-by-character** via `fgetc()`
(`gpx_loader.cpp:172`) into a line buffer, running several `strstr()` calls per line
(`gpx_loader.cpp:178-367`) to drive a hand-written state machine. A verbose geocaching waypoint (full
`groundspeak:` description + hint, up to the 1024-char `desc` cap) spans many lines. Loading 500 such
waypoints at boot means meaningfully more SD/FatFS I/O and string scanning than loading 50 does today.

This is a **one-time cost at boot/refresh**, not a per-frame cost — lower severity than §2-3 by a wide
margin, and the existing UX (a blocking load with serial progress lines, `gpx_loader.cpp:243`) already
tolerates some number of seconds. Still genuinely unmeasured at either count. Worth timing
`loadAllGPXFiles()` before/after on hardware as part of the same field test, not as a blocker.

## Recommendation

**500 is a defensible target** — the SRAM math is clean and lands almost exactly on a headroom level
this project already ran on safely before the PSRAM migration, and nothing else on the roadmap has a
competing claim on that budget. But it should not be adopted as a hardcoded number until two things
happen, in this order:

1. **Implement the §3.6 equirectangular rewrite first**, as its own isolated, low-risk change (it's
   already an approved-in-principle backlog item, independent of any cap decision) — the per-waypoint
   render cost has to come down *before* the candidate count goes up by 10×, not after.
2. **Field-verify `wpt_us`** with a synthetic large waypoint set (a generated 500-waypoint GPX, or a
   real large pocket query if one is available) — both before raising the cap (regression check on
   today's ~9.3 ms "radar paint" baseline) and after, with the fixed-waypoint mode *off* (the loop's
   worst case) and a zoom level where most waypoints are filtered by distance (so the measurement
   reflects the Haversine-replacement cost, not draw-call cost).

Only once `wpt_us` at 500 loaded waypoints is confirmed acceptable should `MAX_WAYPOINTS` actually
move. If it isn't, the fallback isn't "give up 500" — it's a smaller cap chosen from the same
measurement (e.g. 200, which the earlier `AskUserQuestion` round already flagged as the more
conservative option) rather than a second guess.

## Proposed sequencing (no code written yet)

1. `navigation.cpp` — replace Haversine distance/bearing in `drawWaypoints()` (and `latLonToScreen()`,
   which has the same pattern and is also called per-waypoint from `handleTapAt()` per backlog §3.6)
   with the equirectangular approximation; move bearing computation inside the off-screen branch.
2. Field-test: confirm `wpt_us` unchanged-or-better at today's real-world waypoint count (regression
   guard on the rewrite itself, independent of the cap question).
3. `ui_manager.h` — raise `MAX_WAYPOINTS` to the verified target.
4. `settings_screen.cpp` — rescale `updateWaypointCountLabel()`'s color thresholds proportionally to
   the new cap.
5. Build a synthetic large GPX (or source a real large pocket query) and field-test: `wpt_us` at full
   load with fixed-waypoint off, boot/parse time via `loadAllGPXFiles()`, and `memory stats` for the
   live SRAM number to compare against §1's static-link projection.
6. If all three come back clean: update ROADMAP.md (move this entry from Planned to Resolved,
   summary-only, linking here and to CHANGELOG.md), add a CHANGELOG.md entry, and file an ADR — this
   qualifies as a real architectural decision (raise the cap + shrink per-waypoint cost, vs. the
   rejected alternative of keeping the cap low and doing "nearest 500 at load time" filtering on the
   host/GPX side) per CLAUDE.md's documentation standards. Would be ADR-0021 (next unused number as of
   this writing).

## Open question for the user

Real geocaching.com pocket queries can run past 500 caches for a large region. Is 500 meant as "enough
headroom for realistic use" or "the max we can afford" — i.e. is there an appetite to push further
once §3.6 lands and proves the render cost is actually flat, or is 500 a deliberate ceiling regardless
of what the measurement shows?
