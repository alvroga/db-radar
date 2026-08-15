# Quests Feature — Design & Implementation Plan (working draft)

## Context

The roadmap has carried "Quests" as an unscoped brainstorm item since 2026-08-05: a GPX file
containing multiple waypoints that should be tracked and completed as a set, rather than as
independent pins. Nothing was designed or built — the roadmap explicitly called for a scoping
session before any code or ADR. This plan is that scoping session, still in progress; several
items below are marked OPEN and are expected to change.

Quest shape, per the user: a quest is a tagged GPX file with ≥1 waypoints; found = tap-while-
in-range; no ordering requirement; multiple quests can be active at once; quests render like any
other waypoint (no distance-filter interaction); a dedicated quest-tracking screen is real but
lower priority, deferred past this pass.

Three explore agents read the actual GPX/waypoint/index code, the web GPX manager, and the
UI/NVS/asset patterns to find out what those answers cost to build. That surfaced a mismatch
between the user's recollection of the current fix/sonar/found behavior and what the code
actually does (verified directly against `navigation.cpp`, see below), which reshaped the plan:
**proximity sonar and tap-to-confirm are being generalized from "only the fixed waypoint" to
"any waypoint within range," as a standalone navigation change quests then simply inherit** —
rather than quests growing their own separate tap-to-confirm mechanism.

---

## 0. Prerequisite: generalize waypoint proximity (fix / sonar / found)

This is not quest-specific — it's a UX correction to the existing single-fixed-waypoint system
that quests happen to need. Treat it as its own change, landed before or alongside quest work.

**Current behavior, verified against code (not memory):**
- `handleTapAt()` (`navigation.cpp:1413-1431`): tapping the *fixed* waypoint within **15m**
  (not 10m) sets `found = true`, chirps, opens the waypoint detail screen. Does **not** unfix.
- `updateWaypointFixSonar()` (`navigation.cpp:860-935`): proximity sonar only ever targets
  `fixed_waypoint_index`; gated to `ZOOM_50M`; reads `wp.found` to silence itself
  (`:918-923`) — this silencing is what got remembered as "unfixing," but
  `fixed_waypoint_index` is untouched by it.
- Unfixing today only happens via: the waypoint-screen Fix/Unfix button, auto-release at >1km
  (`:1274-1279`), or beacon-priority override (`:881-889`).
- A `waypoint_distance_label` HUD widget already exists (`:1258-1297`, "Fixed: Xm") — currently
  a plain, non-clickable label.
- The proximity star indicator (`:708-737`) is drawn only for the fixed waypoint, at <50m,
  three zone sizes (10m/25m/50m breakpoints).

**Redesign:**
- **Fix = declutter + distance display only.** Hiding other waypoints while fixed already works
  (`:658`, no change). Distance label already works.
- **Tap the distance label to unfix.** New: make `waypoint_distance_label` clickable; on tap,
  clear `fixed_waypoint_index`, stop sonar.
- **Proximity sonar applies to any waypoint within 50m zoom, not just the fixed one.**
  `updateWaypointFixSonar()`'s core logic (distance→tempo mapping, beacon-priority yield,
  found-silencing) is reused, but the target selection changes from "the fixed waypoint" to
  "the nearest in-range waypoint, fixed or not." 50m is purely the sonar gate/tempo range — it
  does not affect whether a tap can mark found.
- **Tap-to-confirm generalizes the same way, and the gate is being tightened: 15m → 10m.**
  Tapping any waypoint (fixed or not) only marks it found if the live GPS distance to it is
  ≤10m — a deliberate reduction from today's fixed-waypoint-only threshold
  (`found_dist <= 15.0f`, `:1421`, changes to `10.0f`), no longer conditioned on
  `i == ui.fixed_waypoint_index`. **RESOLVED 2026-08-06**: chosen over 5m or leaving it at 15m
  because 10m already exists as a breakpoint in the proximity-star zone tiers (10m/25m/50m,
  `:708-737`) — tightening to it reuses an existing constant instead of introducing a new magic
  number, and stays above realistic outdoor GPS noise (~3-5m CEP, worse under canopy) where 5m
  risked frustrating near-misses. Being anywhere in the 50m sonar range is not sufficient by
  itself; the tap only confirms inside 10m.

**RESOLVED — sonar target selection:** presence of *any* waypoint within 50m gates the sound on;
tempo is driven by `min(distance)` across all in-range waypoints, re-evaluated continuously. This
is the same rule as "closest wins" (mirroring the closest-per-sector pattern already used for
off-screen clustering, `:758-761`), just framed as "range gates, nearest-distance modulates" —
whichever one is currently nearest naturally drives the tempo each update, with no separate
target-lock step. This is independent of tap-to-confirm: which waypoint gets marked found is
decided by the existing screen-coordinate hit-test in `handleTapAt()` (whichever pin was actually
tapped), not by proximity ranking — no conflict even if two waypoints both sit inside the 10m
confirm radius at once.

**RESOLVED 2026-08-06 — proximity star generalizes too.** The pulsing zone star is no longer
reserved for the explicitly-fixed waypoint; it follows the same "nearest in-range waypoint" target
selection as sonar, so fixing a waypoint is purely a UX convenience (declutter + distance label +
unfix-by-tap), not a precondition for any proximity feedback.

---

## 0.5 Prerequisite: `gpx_index` file-capacity headroom

Not quest-specific in origin — `gpx_index` (the two-tier waypoint index, ADR-0023, built
2026-08-05) predates the quest brainstorm entirely and was never discussed in it before this pass.
It resurfaced 2026-08-06 while scoping quest capacity, because quests turn out to be the worst
case for one of its two independent budgets.

**The two budgets, verified against `include/gpx/gpx_index.h`:**
- `MAX_INDEX_ENTRIES = 8192` — one slot per **waypoint**, shared across every file.
- `MAX_INDEX_FILES = 64` — one slot per **GPX file**, regardless of how many waypoints it holds.
  A quest file with 10 waypoints costs exactly the same 1 file-slot as a 500-waypoint pocket
  query.

**Problem — quests are file-level (§2), so they hit the file budget, not the entry budget:**

| File type | Typical waypoints/file | Files to exhaust 8192 entries | What actually binds |
|---|---|---|---|
| Heavyweight geocaching pocket query | ~500 | ~16 | Entry cap (16 ≪ 64 files) |
| Lightweight personal waypoint sets | ~50 | ~164 | File cap (64×50 = 3,200 entries — 39% of budget, then hard stop) |
| Quests (one small file per quest) | ~5 | ~1,638 | File cap, massively (64×5 = 320 entries — **3.9%** of budget, then hard stop) |

At today's cap, a user could have thousands of unused entry-slots left in PSRAM and still be
refused the next quest purely because file-slots ran out. This is a real bug-in-waiting for
quests specifically, caught only because capacity was checked before implementation, not after.

**RESOLVED 2026-08-06 — first pass: raise `MAX_INDEX_FILES` 64 → 254, keep `file_id` as
`uint8_t`.** `file_id`s are assigned sequentially from 0 (`gpx_index.cpp` `addFile()`).
`gpx_loader.cpp:279` and `:466` both use `uint8_t open_file_id = 0xFF` as a sentinel meaning "no
file open yet." At a cap of 256, the 256th file would get `file_id = 255 = 0xFF`, colliding with
that sentinel — its waypoints would silently misbehave in the materialize pass. 254 kept every
real id in `[0, 253]`, safely clear of the sentinel, enforced via `static_assert`. This closed the
immediate collision risk, but left `uint8_t`'s 254-id ceiling as a hard wall — any future growth
past it would hit the exact same collision problem again, with no headroom to expand into.

**REVISED 2026-08-07 — widen `file_id` to `uint16_t`, raise `MAX_INDEX_FILES` to 512.** Reopened
because 254 was itself an unexamined byte-frugality habit, not a hardware limit — the ESP32-S3
(Xtensa LX7) is a 32-bit core with 32-bit registers; loading/comparing a `uint8_t`, `uint16_t`, or
`uint32_t` costs the same one cycle. There is no CPU ceiling anywhere near this range. The only
real question was PSRAM cost, so it was measured directly (compiled `IndexEntry` with each
candidate type and checked `sizeof`/`offsetof` on this toolchain's alignment rules — float/
`uint32_t` members force 4-byte struct alignment, matching what `gpx_index.cpp` already assumes):

| `file_id` type | `sizeof(IndexEntry)` | Extra PSRAM (×8192 entries) |
|---|---|---|
| `uint8_t` (254-file plan) | 16 B | — |
| `uint16_t` | 16 B | **+0 B — absorbed by existing padding** |
| `uint32_t` | 20 B | +32,768 B |

`IndexEntry` already pads `file_id`+`found` out to a 4-byte boundary, so `uint16_t` is a **free**
upgrade — it fills padding that's already being spent and does nothing today. `uint32_t` would
cost 32KB for a ceiling (4 billion files) nobody will ever approach; `uint16_t`'s own ceiling
(65,535 files) is already three orders of magnitude past anything this project's storage could
hold, so there's no reason to pay for the bigger type.

```cpp
constexpr int MAX_INDEX_FILES = 512;
static_assert(MAX_INDEX_FILES < 0xFFFF, "0xFFFF is gpx_loader.cpp's open_file_id sentinel");
```
`gpx_loader.cpp`'s two sentinels become `uint16_t open_file_id = 0xFFFF;`. This makes the cap a
pure capacity/PSRAM-cost choice again (matching how `MAX_INDEX_ENTRIES` already works), not
something dictated by an accidental type ceiling. 512 was chosen as a round number well past any
realistic quest/waypoint file count, not because it's close to any limit — PSRAM cost and the
graceful-degradation bounds check (below) mean there's no cliff to size against, unlike
`MAX_WAYPOINTS`'s real SRAM boot-failure cliff in ADR-0022.

PSRAM cost at the new numbers: entries table unchanged (`uint16_t` is free, per above); file
table = 512 × `sizeof(IndexFile)` (96B) = 49,152B (~48KB) vs the original 64-file baseline's
6,144B (~6KB) — a ~42KB delta, trivial against PSRAM's already-abundant budget (ADR-0001/
ADR-0023 framing).

**`MAX_INDEX_ENTRIES` is untouched by any of this — still 8192, and still an independent
budget.** `file_id` is only a per-file tag stamped onto each waypoint entry; widening its type or
raising the file count has no effect on how many total waypoints the entry table holds. The two
budgets still fail independently in either direction — many nearly-empty files can exhaust the
file budget while entries sit mostly unused, or a few huge files can exhaust entries while the
file budget sits mostly unused — which is the mismatch this section exists to track.

**Checked, no other landmine found — for both the cap raise and the type widening:** `file_id`
is used *only* inside `gpx_index.{h,cpp}` and 5 sites in `gpx_loader.cpp` (forward decl,
`addFile()`/`buildFileIndex()` call, and the two sentinel blocks) — no other fixed-size array or
format string is keyed to it, and it's never serialized over the web API (the `/list` endpoint
identifies files by filename via `readdir()`, not by `file_id`), so widening the type doesn't
touch `gpx_server.cpp` at all. The web `/list` endpoint (`gpx_server.cpp:1221`) already streams
per-file via `httpd_resp_send_chunk`, no fixed buffer, scales to any count. FFat/FAT32
subdirectories handle far more than 512 entries. One pre-existing, unrelated-but-adjacent
observation: `/list` reads the FFat directory directly via `readdir()`, independent of
`gpx_index`'s cap — so today, files beyond whatever the index could track are already invisible
to the on-device index while still listed on the web page. Raising the cap narrows that gap,
doesn't create it.

**One real, unmeasured cost (not a blocker):** boot/full-rescan I/O time scales with how many
files *actually* exist, not with the ceiling itself. Never measured at 200+ small files — the
stress test (see Verification) is the first real data point, and it's a single measurement at the
target count, not a search for a breaking point: PSRAM cost is cheap and `addFile()` already
degrades gracefully past the cap (returns -1, logs, doesn't crash), so unlike ADR-0022's
`MAX_WAYPOINTS` there's no hidden allocation cliff to hunt for.

**REJECTED — separate `quests/` and `waypoints/` indexes (two independent PSRAM pools).** Same
failure mode ADR-0023 already rejected once for alternative (b), "per-file caps": that was
*geography*-blind ("a single dense local file would starve out every other file's waypoints");
splitting the full index by category would be *category*-blind the same way — whichever pool
fills first wins, regardless of which files are actually more relevant, and a fixed split can't
adapt to a usage ratio (quest files vs. regular files) nobody knows yet. It would also add real
complexity: since quest waypoints must compete with regular ones for the same 200-slot working
set purely by distance (§0 — quests render like any other waypoint, no distance-filter
interaction), `reselect()` would have to merge-sort across two PSRAM pools instead of one, just
to get back to behavior the single shared index already gives for free. One shared index, bigger
cap — chosen.

---

## Design (quest-specific)

### 1. Quest tagging in GPX (file-level metadata)

A GPX file becomes a quest via one `<extensions>` block containing
`<quest:group>Name</quest:group>`, placed once (in `<metadata>` if present, otherwise anywhere
before the waypoints — the parser scans regardless of position, files are small). Mirrors the
existing `<groundspeak:name>` handling in `gpx_loader.cpp` rather than a filename convention,
which would collide with the `GCxxxx.gpx` convention geocaching already relies on.

**Parsing**: `gpx_loader::buildFileIndex()` already does one fast line-by-line pass per file to
find `<wpt lat=.. lon=..>` and record `{lat, lon, file_offset, file_id}` — it doesn't parse any
field today. Extend that same pass to also:
- match `<quest:group>...</quest:group>` once per file, store the parsed name (see §2), and
- capture each waypoint's `<name>` value into a lightweight hash — the stable ID persistence
  needs (see §3). `file_offset` is *not* stable across a re-upload/edit of the same file; the
  waypoint's own `<name>` (GC code) already is, by convention.

### 2. Data model — extend `gpx_index`, not `ui_manager::Waypoint`

Quest membership is a **file-level** property; the existing `file_id`
(`IndexEntry.file_id`, `include/gpx/gpx_index.h`) is already the right grouping key.

```cpp
// gpx_index.h
struct IndexFile {
    char name[96];
    bool is_quest = false;
    char quest_name[48] = {};
};

struct IndexEntry {
    float    lat, lon;
    uint32_t file_offset;
    uint16_t file_id;         // widened from uint8_t 2026-08-07 — see §0.5, free (padding-absorbed)
    bool     found;
    uint32_t name_hash = 0;   // new — FNV-1a of <name>, stable persistence key
};
```

New query helpers in `gpx_index.cpp`: `isQuestFile(file_id)`, `getQuestName(file_id)`,
`getQuestProgress(file_id) -> {found, total}` (scans entries filtered by `file_id`, counts
`.found` — infrequent call, not hot-path, no new PSRAM structure needed just for the count).

No change needed to `ui_manager::Waypoint` itself — quest membership resolves via the existing
`g_slot_source[slot] → entry → file_id` indirection already used by `markWaypointFound()`.

### 3. Tap-to-confirm — inherited from §0, not separate quest code

Because §0 generalizes tap-to-confirm to any in-range waypoint, quest waypoints get "tap in
range to mark found" for free — no quest-specific tap branch needed in `handleTapAt()`. The
only quest-specific hook is **after** the existing found write-through
(`gpx_loader::markWaypointFound`): check `gpx_index::isQuestFile()` for the tapped waypoint's
file, and if so call `getQuestProgress()`; if `found == total`, fire the completion path (§5).

### 4. Persistence (NVS, quest-scoped)

Scope: persist found-state for **quest-tagged waypoints only** — not a general found-state
persistence system for every waypoint (general found-state has the same gap today — resets on
every rescan/reboot — but that's explicitly not this pass's problem to fix).

- New namespace, following `settings_manager.cpp`'s raw ESP-IDF `nvs_*` idiom (no Arduino
  `Preferences`; no existing array/blob NVS pattern in this repo, so this is new but
  consistent).
- Key per quest = **quest filename** (stable across reboots; `file_id` is not — it's assigned by
  scan order, can shift if files are added/removed).
- Value = blob of found `name_hash`es for that quest (u32 array via `nvs_set_blob`/
  `nvs_get_blob`).
- On boot, after `buildFileIndex()` runs, for each quest file load its persisted blob and set
  `IndexEntry.found = true` for any entry whose `name_hash` matches.

### 5. Completion feedback

**RESOLVED 2026-08-06 — text placeholder now, icon later.** For this pass: `buzzer::doubleBeep()`
plus a temporary `lv_label` reading "Quest Completed" (or similar — exact copy TBD) on the radar
screen, same construction as the existing FOUND banner in `waypoint_screen.cpp`, auto-deleted via
`lv_timer`. Zero new infrastructure. The pixel-art icon idea isn't dropped — it's superseded by
the badge system (§7 below), which now has firmer headroom to justify it, so the pop-up will grow
an icon once badge decoding exists rather than as a standalone effort.

### 6. Web GPX manager (`src/gpx/gpx_server.cpp`)

- Upload flow gets one optional text input, "Quest name" (in the existing `.upload-area` div).
  Passed as an extra query param on the existing raw-body `/upload?filename=&quest=` POST — no
  multipart/form-data rework needed, the endpoint is already query-param based.
- `upload_handler` injects the
  `<metadata><extensions><quest:group>...</quest:group></extensions></metadata>` block
  server-side if the param is present and the uploaded file doesn't already have one (so a file
  authored elsewhere with the tag already in it still works without the upload-page field).
- `/list` response gains `is_quest`, `quest_name`, and `found`/`total` progress per file, sourced
  from the new `gpx_index` helpers.
- `UPLOAD_HTML`'s file-list rendering gets a small quest badge + "X/N found" next to quest
  entries.

### 7. Badges (quest completion reward) — raised 2026-08-06, needs its own scoping pass

Concept: completing a quest doesn't just fire a one-shot toast (§5) — it unlocks a small icon
("badge") tied to that quest's name, kept permanently as a collection. Author's intent: the badge
should be as simple as possible to *create* — small enough to write by hand as a hex string
directly inside the quest's GPX file, no build tooling required, mirroring how `<quest:group>`
already keeps quest authoring to "edit a text file."

**RESOLVED 2026-08-06 — icon format, validated against a real reference badge, then tested.** User
supplied an actual pixilart-style achievement badge; measured directly (PIL): **32×32 px, RGBA, 6
unique colors** (transparent + white + near-black + 3 shading tones), 52% transparent pixels. That
established the *shape* of the format:
- **4bpp / 16-color indexed palette, with one slot reserved transparent.** LVGL's indexed formats
  only exist in 1/2/4/8-bit steps; the reference badge's 6 colors don't fit in 2-bit (4 colors),
  so 4-bit is the floor, not a stylistic pick — and 16 slots against real-world 4-6 colors used
  leaves headroom for more elaborate badges without a format change.
- Model is SNES-style palette discipline (small fixed per-sprite palette + index-per-pixel), not
  SNES-style tile/OAM composition — no sprite-layering or tilemap system is needed, just this one
  static indexed bitmap per badge. Maps directly onto LVGL's native `LV_IMG_CF_INDEXED_4BIT`,
  where the palette is stored immediately before the pixel data in the same buffer — no custom
  on-device decoder beyond "parse hex into that byte layout."

**RESOLVED 2026-08-06 — resolution: 24×24 is the standard, not 32×32.** Two real test badges were
built to check whether shrinking below the 32×32 reference loses legibility: a broad-shape badge
(5-point star, circular ring) and a controlled same-glyph test at both sizes using 百 ("hundred") —
deliberately a harder case than the star, dense fine strokes rather than a broad shape. Both stayed
legible at 24×24; 百 got a bit blockier (less breathing room between strokes) but remained
unambiguously readable. Since storage was never the real constraint (§0.5/below confirm this at
either size), the choice came down to visual quality only, and 24×24 held up even for the hard
case — so it's the default. Sizes at 4bpp:
- **24×24 (standard): 288 bytes pixel data (576px × 4 bits) + 32 bytes palette (16 × RGB565) =
  320 bytes = 640 hex characters.**
- **32×32 (optional, larger tier): 512 + 32 = 544 bytes = 1,088 hex characters.** Kept available
  for a rare/legendary badge that genuinely needs more room — two overlapping design elements, not
  just one dense glyph — not as the default.
- RGB565 matches the display's native pixel format directly, no per-draw conversion, at either
  size.

**Encoding**: a second tag alongside `<quest:group>` in the same `<extensions>` block, e.g.
`<quest:badge>`, containing the hex string above. **§1's `buildFileIndex()` extension already
visits `<extensions>` once per file** (it's adding `<quest:group>` parsing there this pass) —
`<quest:badge>` would piggyback on that same scan rather than being a second pass.

**RESOLVED 2026-08-06 — storage, corrected from an earlier wrong assumption.** This section
originally claimed badge pixel data "doesn't need duplicating — it lives in the GPX file already,
re-decoded on demand." **That's wrong and has been superseded**: an earned badge must survive
independent of the source quest file's fate (edited, re-uploaded, or deleted) — it's a trophy, not
a cached view of the file. Re-decoding on demand would mean deleting a completed quest's GPX also
deletes the achievement, which defeats the point of a permanent collection.

Corrected design:
- On quest completion, decode the badge once from `<quest:badge>` and write it as its own file
  into a **new, dedicated `/ffat/badges/` directory** — separate from `/ffat/gpx`, the directory
  `gpx_index::buildFileIndex()` scans.
- **`/ffat/badges/` is deliberately NOT registered with `gpx_index` at all** — no file-table slot,
  no entry-table slots. This keeps badge storage fully decoupled from §0.5's file-capacity budget;
  earning badges can never compete with quest/waypoint file capacity, and vice versa.
- On-disk format: the same 320B (24×24) / 544B (32×32) palette+packed-pixel layout used in the
  `<quest:badge>` hex tag, stored as raw binary — no need to keep the hex-text encoding once it's
  off the human-authored GPX file and onto internal device storage.
- Listing: a plain `readdir()` over `/ffat/badges/` when a badge collection view is opened — no
  PSRAM index needed; realistic badge counts are bounded by however many quest files can ever
  exist (≤512 after §0.5's 2026-08-07 revision), and even that worst case is cheap (below).
- **Recomputed cost, corrected model** (this is now a real allocation, not "free" reuse of the
  source file — but still trivial in absolute terms): worst case, all 512 quest-capable files earn
  a badge → 512 × 320B (24×24, the standard) ≈ 160KB, or 512 × 544B (32×32, the larger tier) ≈
  272KB. Both ≈2–3.5% of the ~7.69MB FFat budget. Storage still isn't the real constraint — it
  just needed the honest mechanism.
- §4's NVS quest-scoped persistence (found `name_hash`es, per-waypoint progress within an
  *in-progress* quest) is unchanged by this — it's still tied to the source GPX file's continued
  existence, for tracking partial progress. The new `/ffat/badges/` file is the separate,
  permanent record created once at completion, independent of §4 from that point on.
- **Collection UI**: naturally belongs on the deferred dedicated quest-tracking screen (see
  "Explicitly out of scope" below) as a badge shelf/gallery — not this pass, but this data model
  (badge is its own file, completion is a bool) shouldn't need to change when that screen gets
  built.

**Open, not yet resolved:**
1. At 640 hex characters (24×24 standard) or 1,088 (32×32 tier), hand-typing a badge is
   unrealistic — this needs a small PNG→hex encoder (script now, ideally folded into the web
   quest/waypoint creator once that's scoped, so authoring is "draw pixels" not "write hex"). Not
   a blocker for the on-device decode/render side, which doesn't care how the hex was produced.
2. Whether §5's completion pop-up icon and the "permanent badge" are literally the same asset
   (one `<quest:badge>` tag drives both) or conceptually separate.
3. Badge filename scheme on `/ffat/badges/` — needs a stable, collision-safe name (candidate: a
   hash of `quest_name`, mirroring §4's `name_hash` pattern for waypoints, or a sanitized slug).
   Not nailed down.
4. No code has been read for this section yet (unlike §0.5 and §1-6, which were verified against
   `gpx_loader.cpp`/`gpx_index.h`/`navigation.cpp`/`gpx_server.cpp`) — treat the storage/parsing
   sketch above as a plausible shape, not a verified plan, until an explore pass confirms it
   against the actual code.
5. **Found 2026-08-07, verified against code**: `buildFileIndex()`'s line reader
   (`gpx_loader.cpp:316`, `char line[512]`) treats anything past 511 bytes as a line break
   (`:326`). Both badge hex tags exceed that — 640 chars (24x24) and 1,088 chars (32x32), before
   tag overhead — so a `<quest:badge>` line will arrive split across 2-3 reads. Harmless today
   (nothing scans for the tag yet), but whoever implements §1's `<quest:badge>` extraction cannot
   reuse the existing single-line `strstr()` pattern as-is — it needs to either accumulate the
   split reads or read the badge tag with a dedicated larger buffer. Confirmed via a 512-file
   stress-test batch with real 640/1,088-char bogus hex payloads (`gpx_test_512_badges.zip`,
   2026-08-07) — file sizes: 893B (24x24) / 1,341B (32x32), ~558KB total for 512 files (256 of
   each), well inside the ~7.69MB FFat budget either way.

---

## 8. Milestone badges — a second badge category, distinct from quest badges

Raised 2026-08-07, brainstorm — not yet verified against code (same caveat as §7 item 4). A quest
badge (§7) is *author-supplied*: art comes from a specific quest's `<quest:badge>` hex tag, unlocked
by completing that one quest. A milestone badge is *system-defined*: no GPX supplies it, because
nothing being tagged owns it — "your first waypoint," "10 waypoints found," "50 waypoints found,"
"waypoints found on 5 continents," "1 quest completed," "10 quests completed," and so on are
properties of the user's cumulative history, not of any one file.

**Same rendering/storage plumbing, different trigger and different art source:**
- **Art ships in firmware, not in a GPX.** There's no quest file to source `<quest:badge>` hex from
  for a milestone badge, so the catalog is a small fixed set of 4bpp indexed images (§7's exact
  format) baked into flash at build time — reuses the same on-device decoder §7 already needs, no
  second image format to support. This also means milestone-badge art is authored once by whoever
  builds the firmware, not per-quest by whichever user wrote that quest's GPX — a real asymmetry
  worth being explicit about, not a flaw.
- **Unlock trigger is a threshold crossing, not a single found-event.** Needs cumulative counters,
  persisted the same way §4 persists quest progress (NVS): total *distinct* waypoints found (must
  increment only on a genuinely new found-event, not on re-tapping an already-found waypoint — the
  existing `IndexEntry.found` flag already distinguishes "already true" from "just became true," so
  the counter increments off that transition, not off every tap) and total distinct quests completed
  (increments off §3's existing quest-completion check, §7's hook point). Same storage shape as an
  earned quest badge once unlocked — written to `/ffat/badges/`, outside `gpx_index`'s file-capacity
  budget (§0.5), for the same "must survive independent of any source file" reason §7 already
  established.
- **Continent coverage is the one genuinely new capability.** Classifying a found waypoint's lat/lon
  into one of ~7 continents needs *some* offline lookup — there's no network geocoding service on
  this device and none should be added just for this. A coarse static bounding-box (or a handful of
  simple polygon) table, checked once per newly-found waypoint and OR'd into a persisted 7-bit
  continent-visited mask (fits in one NVS byte), is enough — this doesn't need survey-grade accuracy,
  only "which of 7 buckets." Same self-contained, no-network spirit as the existing WMM declination
  module (`wmm_declination.cpp`).
- **Trigger hook is the same one §3 already establishes for quest completion** — "after the existing
  found write-through, check ... and fire the completion path" — milestone evaluation is a second
  check at that same point, not a new mechanism: on every newly-found waypoint, increment the
  waypoint counter and update the continent mask; on every newly-completed quest (§7's own hook),
  increment the quest counter; after each increment, check whether any milestone threshold was just
  crossed and if so unlock that badge.

**Open, not yet resolved:**
1. The actual threshold list (1/10/50/... waypoints, which quest counts, which continent counts) is
   a product decision, not an engineering one — needs the user's call, not a default invented here.
2. Whether milestone badges get their own gallery section or share one shelf with quest badges on
   the eventual collection UI (§7's deferred dedicated screen).
3. No code has been read for this section — same caveat as §7 item 4, treat this as a plausible
   shape until an explore pass confirms it against `gpx_loader.cpp`/`gpx_index.h`/`navigation.cpp`.

---

## 9. Route Mode — ordered waypoint navigation (brainstorm, 2026-08-15)

### Context

Raised by the user 2026-08-15 during a redesign conversation about `tools/waypoint-editor/index.html`
(layout + map-tile-theme changes — see the correction to "Future ideas" below, this tool is the
identified home for the unscoped "web waypoint/quest creator" that section already called for). Not
yet verified against code beyond what's cited here; no ADR yet; nothing designed or implemented.

Distinct from §0-§8 in one important way: this project's Context section (top of this doc) explicitly
resolved "no ordering requirement" for quests — a quest is a *set*, found in any order. Route mode is a
new capability layered on top, not a revision of that resolution: **route is an independent flag,
orthogonal to quest, not a replacement for the unordered model.** A GPX file can be:
- **Quest only** (today's design): tracked found/total, no order, no guided navigation.
- **Route only**: ordered navigation (walk to point 1, then 2, then 3...), no found/total tracking or
  completion badge.
- **Route + Quest**: ordered navigation *and* tracked completion/badge on reaching the last point.
- **Neither**: today's plain independent waypoints, unchanged.

### Motivating shape (user's framing)

1. **Web tool**: draw/connect waypoints in visit order (A → B → C) when authoring a GPX.
2. **On-device**: fixing onto a route should (a) declutter the display to *only that route's
   waypoints* — the whole group stays visible, not just the single next target, generalizing what
   "fixed" already does for one waypoint today up to a file — and (b) draw a line from the user's
   current position to the *next* unvisited point in the route.

### GPX representation — recommend a file-level flag, not `<rte>`/`<rtept>`

ROADMAP.md's "GPX Generator Tool" entry already raised this exact fork: GPX's native `<rte>`/`<rtept>`
route element vs. a plain sequential `<wpt>` list (the shape Pokémon GO-style walk files already use).
Recommendation, reasoning from what's already true of the code:

- `gpx_loader.cpp`'s fast index scan (`buildFileIndex()`) already captures `file_offset` per waypoint
  (§1, confirmed against code) — **file order is already recoverable for free** by sorting a route
  file's entries by `file_offset` ascending. No per-waypoint sequence tag is needed just to express
  order.
- The only thing actually missing is *intent* — "this file's `<wpt>` order means something" vs. "this
  is an unordered set that merely happens to have some order in the file." That's one boolean, not a
  new per-point field.
- **Recommendation: reuse the exact `<extensions>` mechanism §1 already established for
  `<quest:group>`/`<quest:badge>`** — add a file-level `<quest:route/>` marker (empty tag, presence =
  true) in the same block, `IndexFile` gains one new `bool is_route`, parsed in the same single
  fast-scan pass. No new tag name for the scanner to match — `<wpt` stays the only per-point tag it
  ever looks for, preserving the property confirmed during this brainstorm that `gpx_loader.cpp` never
  references `<rte>`/`<trk>`/`<rtept>`/`<trkpt>` anywhere.
- Answers the web-tool side too: the tool already builds `waypoints[]` as an array and writes it out
  via `buildGPX()`/`exportAll()` in array order — "route order" is just "array order." A route-editing
  UI (drag-to-reorder, or click-to-connect-in-sequence) needs no separate ordering field in the tool's
  own data model either.
- **Alternative considered and not recommended by default**: native `<rte>`/`<rtept>`. Would require
  `gpx_loader.cpp`'s fast scanner to also match a second top-level tag and track `<rte>` open/close to
  associate `<rtept>` children with their parent's name/order — real parser complexity for something
  `file_offset` already gives for free. Worth revisiting only if interop with externally-authored
  `<rte>` files becomes a real ask — not raised yet.

### On-device behavior — extends the existing fixed-waypoint mechanism, doesn't replace it

Confirmed against code: `docs/waypoint_filtering.md`'s "Touch Interaction and Distance Display" section
states `drawWaypoints()` already enforces "when a waypoint is fixed, render only that target — all
other on-screen dots and off-screen triangles disappear" (single `fixed_waypoint_index int`,
`ui_manager.h:166`). This is the mechanism to generalize, not rebuild:

- **List filtering**: generalize "render only the fixed target" from *one waypoint index* to *one
  file_id's full membership* when the fixed target is a route — filter predicate becomes "waypoint
  belongs to `fixed_route_file_id`" instead of "waypoint index == `fixed_waypoint_index`." The
  `g_slot_source[slot] → entry → file_id` indirection §2 already uses for quest-membership lookups is
  the same lookup this needs.
- **Line-to-next**: technically cheap — `radarDrawEventCb` (`navigation.cpp`) already calls
  `lv_draw_line()` for the grid lines (lines 315-361), same draw context. A route-mode line is one more
  `lv_draw_line()` call per frame: from the user's screen position (center in heading-up; the position
  dot's coords in north-up) to the next un-found route waypoint's already-computed screen (x,y).
  `drawWaypoints()` already computes on-screen coordinates for every visible point, and off-screen
  points already get a clamped edge position for their triangle indicator (sector clustering, see
  `docs/waypoint_filtering.md`) — the line could reuse that same clamped point as its endpoint when the
  next point is off-screen, rather than inventing new geometry.
- **"Next" determination**: sort the route file's entries by `file_offset` ascending, target = first
  entry with `!found`. Recompute after every found-transition, using the same hook point §3 already
  established ("after the existing found write-through, check quest completion") — a route file gets an
  equivalent check ("if this file is a route, recompute current target").
- **Auto-advance**: falls out of the above for free — once the current target's `found` flips true, the
  next un-found entry becomes the new target automatically on the next recompute. No separate "advance"
  state machine needed.

### Open questions — need your call, not resolvable by re-reading code

1. **Auto-release interaction**: `FIXED_WAYPOINT_MAX_DISTANCE_M` (100km) auto-releases a *stale*
   single-waypoint fix. For a route, does the same numeric check apply to just the *current target*
   (recomputing release-eligibility every time the target advances), or should a deliberately-fixed
   route never auto-release (a multi-point route being an intentional plan, not the accidental stale
   fix the safety net exists to catch)?
2. **Completion behavior for a route that isn't a quest**: reaching the last point of a route-only
   (non-quest) file — silent unfix back to normal view, or some lighter feedback (chirp/toast) distinct
   from §5's quest-completion pop-up?
3. **Off-screen next-point at high zoom**: if the next route point is well outside the current zoom
   radius, force a temporary zoom-out to keep it visible, or is pointing the line at the clamped
   off-screen edge (as sketched above) enough?
4. **Reordering an already-fixed route mid-walk**: almost certainly out of scope for v1 (routes are
   authored before a walk, not edited live) — stated explicitly so it isn't silently assumed later.
5. Not yet verified against code the way §0-§6 were (same caveat §7/§8 carry): this section is a
   plausible shape reasoned from confirmed facts (`file_offset` capture, `lv_draw_line` usage, the
   existing fixed-waypoint filter), not something an explore pass has independently checked line-by-line.

---

## Design philosophy: web = preparation, radar = the vessel

Raised 2026-08-06, framing note for all quest-related web work (§6, §7, and the future-ideas
items below), not a new technical decision. The device's round 480×480 display is a poor place to
read text or do rich input — so the intent is to push as much of the "game" (browsing quests,
picking what to load, seeing badges collected, authoring new content) into the web UI, and keep
the radar itself as the in-field execution surface: walk, watch the sonar/star, tap to confirm.
**The existing web GPX manager (§6) and the not-yet-built web quest/waypoint creator (future
ideas, below) need to share a visual identity** — same look and feel — so moving between "manage
what I have" and "create something new" reads as one continuous tool, not two unrelated pages.
This has no code implication yet; flagging it now so the creator (when scoped) is designed to
match §6's existing page rather than drifting into its own style.

**Extended 2026-08-15 — the shared identity is now a specific direction, and it covers a third
tool.** The GPX generator (`tools/waypoint-editor/index.html`, aka the creator referenced above —
see the "Future ideas" correction earlier in this doc) got a pastel Dragon-Ball-inspired palette
mockup approved (warm gi-orange + dragon-ball gold accents, sky-blue for links/routes, muted
coral-red for destructive actions, cream/parchment panels, bounded rounded-corner "window" panels
instead of edge-to-edge fills) — see `CHANGELOG.md`'s 2026-08-15 entry and `ROADMAP.md`'s "GPX
Generator Tool" entry for the mockup/implementation detail. **Decision: this palette and layout
language is the target for all three of the project's web-facing tools**, not just the generator:

1. **`tools/waypoint-editor/index.html`** (GPX generator/creator) — layout + tile provider shipped
   2026-08-15; color reskin approved, not yet applied (waiting on route/quest UI to exist before
   skinning route/badge chips — see ROADMAP.md).
2. **`src/gpx/gpx_server.cpp`'s `UPLOAD_HTML`** (on-device GPX manager, §6 above) — currently a
   different, older dark-terminal look (`#1a1a1a`/`#2a2a2a` panels, bright `#00ff00` green, "DRAC OS
   GPX Upload" branding) — not yet touched.
3. **`web/flasher/index.html`** (browser web flasher, `docs/firmware_installation.md`) — also
   currently a dark-terminal look (`#0a0f0a`/`#10160f`, `#39d353` green, "DRAC OS — Web Flasher"
   branding), a third and slightly different variant of the same green-on-black family — not yet
   touched.

Not yet implemented on either #2 or #3 — recorded here as a firm direction, not a request to change
them without a further go-ahead, since both are load-bearing utility pages (one embedded in the
firmware image, one gating how users flash the device) where a botched reskin has higher stakes than
the standalone generator tool.

---

## Explicitly out of scope for this pass

- **Dedicated quest-tracking screen.** `waypoint_screen.cpp` (single self-contained screen,
  follows the load-then-delete lifecycle rule) and the scrollable flex-row list pattern in
  `settings_screen.cpp` are the right templates when this gets built. Noted so the data model
  above doesn't need to change later, not because it's deferred without a plan.
- **General (non-quest) found-state persistence.** Deliberately scoped to quest waypoints only;
  see §4.
- **Coordinate-spoofing / "found" authenticity ("cheating").** Raised 2026-08-07: nothing stops a
  user from hand-editing a quest GPX (or a personal waypoint file) to move a tagged waypoint's
  `<name>` next to themselves, then tapping it within range for an instant, travel-free found/badge
  — the file is plain-text and user-editable by design (§7's own authoring goal: "as simple as
  possible to *create*... edit a text file"), so anything that hardens coordinate authenticity
  directly fights the thing that makes quests easy to author. **Deliberately not defended against.**
  There is no server, no account, and no other player whose stakes this affects — the entire trust
  boundary is the user's own device, so this is structurally the same as real-world geocaching's
  long-tolerated "armchair logging," except lower-stakes still (single-player, no shared log).
  Revisit only if this device ever grows a networked/shared/leaderboard feature, since that's the
  point a third party's stakes would actually be at risk — no such feature exists or is planned.

---

## Future ideas — quest creation & manager improvements (brainstorm, not scoped)

Raised 2026-08-05, after the rest of this doc was drafted. Not designed, not costed against the
code, no files read for this section yet — recorded here so it isn't lost, not because it's ready
to build. Resolve into a real §-numbered design (with code references, like the rest of this doc)
before implementation.

**Quest creation — RESOLVED 2026-08-06 (location, not design): lives in the web waypoint
creator, not a bolt-on to §6's upload form.** §6 only covers *tagging an existing GPX file* as a
quest at upload time. Authoring a new quest from scratch (new waypoints — name/lat/lon/description
per waypoint — plus the `<quest:group>` and `<quest:badge>` tags) belongs in a general-purpose web
GPX/waypoint creator — quests are one option within it, not a separate tool.

**Correction, 2026-08-15: that creator already exists.** The line above ("doesn't exist yet in any
form") was wrong — `tools/waypoint-editor/index.html` (Leaflet-based, deployed standalone at
https://alvroga.github.io/gpx-generator/, tracked separately in ROADMAP.md's "GPX Generator Tool"
entry) is a working tap-a-map waypoint creator with name/display-name/desc/hint fields, GPX
import/export, and its own dark/monospace visual identity — it just isn't quest- or route-aware
yet, and its layout has no room for the controls that would need. A 2026-08-15 redesign pass is in
progress on that file covering: reclaiming map space for a wider single-column control panel, a
non-dark map tile theme (a Google-Maps-style light basemap, to be picked — MapTiler Streets or Esri
World Street Map were the live candidates), Route Mode authoring (§9 above), and room for a badge
picker/preview (§7). This resolves the "how are waypoints entered" question above — tap-a-map,
already built — and means this creator and §6's existing device-hosted manager are two separate
pages (one standalone/GitHub-Pages, one on-device), not the same file; "look like one tool" (Design
philosophy, above) still applies to shared visual identity, not shared code. Still open: whether
this tool reuses `upload_handler`'s `<extensions>` injection pattern or builds the
`<quest:group>`/`<quest:badge>`/`<quest:route>` tags directly client-side the way it already builds
`<wpt>` (latter is more likely, since it's a static page with no server-side upload step of its own).

**Manager needs more visibility, not just upload/tag — confirmed direction, still fuzzy on
specifics.** The current `/list` + `UPLOAD_HTML` surface (§6) is upload-and-badge only. Confirmed:
more visibility is worth building. Still open: richer per-quest info in the web UI — progress
(already planned, §6's "X/N found"), which waypoints specifically are found vs. outstanding, a
lat/lon map view of the quest's waypoints, and now also a badge collection view (§7). Needs a
brainstorm pass on what's actually useful to see there before it becomes a design.

Both of these are extensions of the same underlying tool (the web GPX manager), so they likely
land together: a better authoring/management surface for quests, not just the tag-on-upload path
in §6. Revisit once §0–§0.5 and §1–§7 above are built and there's a real quest to manage.

---

## Open decisions to resolve before implementation

All of §0, §0.5, and §1-6's original open decisions are now resolved (star generalizes, tap
radius is 10m, completion feedback is text+chirp for this pass, `file_id` widened to `uint16_t`
and file cap raised to 512, badge resolution is 24×24, badge storage is a separate
`/ffat/badges/` file). What's still open:

1. Whether the completion pop-up icon and the permanent badge are the same asset (§7.2).
2. Badge filename scheme on `/ffat/badges/` (§7, item 3).
3. §7 hasn't been verified against actual code yet — needs the same explore-agent pass §0.5 and
   §1-6 got.
4. Web waypoint/quest creator — not scoped at all yet beyond "it should exist, quests are an
   option within it, and it's the natural home for a badge pixel-editor" (future ideas section).
5. Web manager visibility improvements (found/outstanding breakdown, map view, badge gallery) —
   confirmed direction, no concrete design (future ideas section).
6. **Execution risk, not a design gap**: `MAX_INDEX_FILES = 512` (with `file_id` as `uint16_t`)
   and 24×24 badges are design decisions, not yet field-tested. The planned stress test
   (Verification, below) is the first real data point on boot/rescan time at high file counts. If
   it breaks, the fallback is lowering `MAX_INDEX_FILES` — nothing else in the design depends on
   the specific value beyond the `static_assert`'s `< 0xFFFF` bound.
7. **New, 2026-08-07**: milestone-badge threshold list (§8, item 1) — a product decision, not
   proposed here.
8. **New, 2026-08-07**: §8 (milestone badges) hasn't been verified against actual code yet — same
   gap as item 3 above, now applies to two sections instead of one.

---

## Files to touch

- `include/gpx/gpx_index.h` / `src/gpx/gpx_index.cpp` — **§0.5, DONE 2026-08-07**:
  `MAX_INDEX_FILES` 64 → 512, `IndexEntry::file_id`/`addEntry()`/`getFileName()` widened
  `uint8_t` → `uint16_t`, `static_assert(MAX_INDEX_FILES < 0xFFFF, ...)` against the
  `open_file_id` sentinel. `pio run` clean, RAM/Flash static usage unaffected (the file table is
  PSRAM-only). Still open: `IndexFile`/`IndexEntry` extensions, quest query helpers, NVS load/
  save for quest found-hashes.
- `src/ui/navigation.cpp` — §0 generalization (sonar + star target selection to nearest in-range
  waypoint, clickable distance label, 10m tap-confirm gate); quest-completion check after the
  existing found write-through; completion pop-up (§5, text placeholder this pass).
- `src/gpx/gpx_loader.cpp` — **§0.5, DONE 2026-08-07**: both `open_file_id` sentinels widened to
  `uint16_t open_file_id = 0xFFFF`, `buildFileIndex()`'s signature widened to match. Still open:
  extend the fast scan for `<quest:group>` + `<quest:badge>` + `<name>` hashing; wire found-state
  persistence into `markWaypointFound()`; **new**: on quest completion, decode `<quest:badge>`
  and write it to `/ffat/badges/` (§7).
- `src/utils/diagnostics.cpp` — **new, DONE 2026-08-07**: `gpx index genfiles <lat> <lon>
  [count]` / `genfiles clean` debug commands, mirroring `gentest`'s pattern but generating up to
  600 *separate* files (1 waypoint each) instead of many waypoints in one file — `gentest` only
  ever exercised `MAX_INDEX_ENTRIES`, never `MAX_INDEX_FILES`, so there was no existing tool that
  could stress-test the file-count cap itself. Times the reload and reports index-truncation
  state, for the hardware stress test below.
- **New**: badge file I/O (write-on-completion, `readdir()`-based collection listing) — home TBD,
  likely a small new module (`gpx_badges.cpp`?) or folded into `gpx_loader.cpp`; not yet decided
  which file owns this.
- `src/gpx/gpx_server.cpp` — upload param handling, `<extensions>` injection, `/list` response
  fields, `UPLOAD_HTML` quest name field + badge/progress display. Web waypoint/quest creator and
  richer manager visibility (future ideas section) are separate, unscoped work in this same file.

## Documentation (per this project's standards)

New subsystem → CHANGELOG.md entry, a `docs/quests.md` component doc, an ADR for the
tagging-mechanism decision (extensions tag vs. filename convention vs. separate manifest — a
real alternative was rejected), likely a second ADR for the fix/sonar generalization (a real
behavior change to an existing, previously-deliberate design), and likely a third ADR for the
`gpx_index` file-capacity raise + rejected quests/waypoints split, including the `uint8_t` →
`uint16_t` `file_id` widening and its measured zero-cost padding finding (§0.5 — two real
alternatives, citing ADR-0023's precedent and a direct struct-layout measurement, were rejected/
superseded). ROADMAP.md status update once built.

## Verification

- `pio run` clean build — **done 2026-08-07**, `uint16_t` widening + `MAX_INDEX_FILES = 512`
  compiles clean, `static_assert` passes, static RAM/Flash unaffected.
- **Stress-test `MAX_INDEX_FILES = 512` with up to 512 small quest-shaped GPX files** (few
  waypoints each, matching §0.5's "quests are the worst case" scenario) on real hardware — **not
  yet run, next step.** Use the new `gpx index genfiles <lat> <lon> 512` debug command (writes
  512 real 1-waypoint files, times the reload, reports `MAX_INDEX_ENTRIES`-truncation state); a
  second run at `count=600` exercises the past-512 "file table full" degrade path on purpose.
  Watch boot/full-index-scan time (never measured at this file count) and confirm no correctness
  regression (no `file_id`/sentinel collision at the `uint16_t` boundary, `/list` still correct,
  working-set reselect still picks the true closest 200). Clean up with `gpx index genfiles
  clean` when done. **If anything breaks, roll back `MAX_INDEX_FILES` to a lower value** —
  nothing else in the design is hard-coded to 512 specifically, and `uint16_t` leaves headroom to
  land anywhere below it without another type change.
- Hand-author a synthetic quest GPX (multiple `<wpt>` + the extensions tag), respecting the
  parser's one-element-per-line assumption (a prior test-file generator violated this during the
  two-tier index verification pass — same constraint applies here).
- Upload via the web manager; confirm `/list` shows the quest badge and 0/N progress.
- On hardware: verify the generalized proximity behavior first (§0) independent of quests — walk
  near a non-quest waypoint, confirm sonar+tap-confirm work without fixing it. Then tap each
  quest waypoint in range, confirm `found` sets and persists through the existing
  `gpx index list`-style diagnostic command; confirm completion feedback fires on the last one;
  reboot the device and confirm progress is still there.
- **New**: complete a quest end-to-end, confirm a badge file appears in `/ffat/badges/`; delete
  or re-upload the source quest GPX and confirm the badge survives (the whole point of §7's
  corrected storage design).
