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
- **Tap-to-confirm generalizes the same way, but keeps its own separate, stricter gate: 15m.**
  Tapping any waypoint (fixed or not) only marks it found if the live GPS distance to it is
  ≤15m — same hard threshold as today's fixed-waypoint-only code (`found_dist <= 15.0f`,
  `:1421`), just no longer conditioned on `i == ui.fixed_waypoint_index`. Being anywhere in the
  50m sonar range is not sufficient by itself; the tap only confirms inside 15m. This is not the
  user's recalled 10m — verified against the actual constant.

**RESOLVED — sonar target selection:** presence of *any* waypoint within 50m gates the sound on;
tempo is driven by `min(distance)` across all in-range waypoints, re-evaluated continuously. This
is the same rule as "closest wins" (mirroring the closest-per-sector pattern already used for
off-screen clustering, `:758-761`), just framed as "range gates, nearest-distance modulates" —
whichever one is currently nearest naturally drives the tempo each update, with no separate
target-lock step. This is independent of tap-to-confirm: which waypoint gets marked found is
decided by the existing screen-coordinate hit-test in `handleTapAt()` (whichever pin was actually
tapped), not by proximity ranking — no conflict even if two waypoints both sit inside the 15m
confirm radius at once.

**OPEN — needs a decision before implementation:**
1. **Does the proximity star generalize too?** i.e. does *any* nearby waypoint show the pulsing
   zone star, or does the star stay reserved for the explicitly-fixed one while sonar/tap-confirm
   go general? Not yet decided.

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
    uint8_t  file_id;
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

**OPEN — reconsider vs. earlier default.** Original default (text + chirp, zero new
infrastructure) still stands: `buzzer::doubleBeep()` plus a temporary `lv_label` on the radar
screen (same construction as the existing FOUND banner in `waypoint_screen.cpp`, auto-deleted
via `lv_timer`). But LVGL pixel art turns out to be cheap — a small indexed-format icon (e.g.
32×32 1-bit = 128 bytes) can be hand-authored as a `const lv_img_dsc_t` living in flash, no
conversion tool or PSRAM/heap allocation needed. Worth a real decision, not defaulting to text
just because "image" sounded expensive — it isn't, at this size.

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

---

## Explicitly out of scope for this pass

- **Dedicated quest-tracking screen.** `waypoint_screen.cpp` (single self-contained screen,
  follows the load-then-delete lifecycle rule) and the scrollable flex-row list pattern in
  `settings_screen.cpp` are the right templates when this gets built. Noted so the data model
  above doesn't need to change later, not because it's deferred without a plan.
- **General (non-quest) found-state persistence.** Deliberately scoped to quest waypoints only;
  see §4.

---

## Future ideas — quest creation & manager improvements (brainstorm, not scoped)

Raised 2026-08-05, after the rest of this doc was drafted. Not designed, not costed against the
code, no files read for this section yet — recorded here so it isn't lost, not because it's ready
to build. Resolve into a real §-numbered design (with code references, like the rest of this doc)
before implementation.

**Quest creation in the web GPX manager.** §6 above only covers *tagging an existing GPX file*
as a quest at upload time (a "Quest name" field on top of the existing upload flow). It does not
cover *authoring* a new quest from scratch in the browser — i.e., a flow to create a new GPX file
with new waypoints (name/lat/lon/description per waypoint) and the `<quest:group>` tag, without
the user having built the GPX file externally first. Needs its own scoping pass: how are
waypoints entered (manual lat/lon fields? tap-a-map?), does it reuse `upload_handler`'s
`<extensions>` injection or need a new "create" endpoint, how many waypoints is reasonable to
author through a form on an ESP32-hosted page.

**Manager needs more visibility, not just upload/tag.** The current `/list` + `UPLOAD_HTML`
surface (§6) is upload-and-badge only. Idea, still fuzzy: richer per-quest info in the web UI —
progress (already planned, §6's "X/N found"), but also things like which waypoints specifically
are found vs. outstanding, and possibly a map view of the quest's waypoints (plotted against
lat/lon, not necessarily live device position). Needs a brainstorm pass on what's actually useful
to see there before it becomes a design — this is the open half of the idea, not a decision.

Both of these are extensions of the same underlying tool (the web GPX manager), so they likely
land together: a better authoring/management surface for quests, not just the tag-on-upload path
in §6. Revisit once §0–§6 above are built and there's a real quest to manage.

---

## Open decisions to resolve before implementation

1. Does the proximity star generalize to any nearby waypoint, or stay fixed-only (§0)?
2. Completion reward: text+chirp vs. a small hand-authored pixel-art icon (§5).

---

## Files to touch

- `src/ui/navigation.cpp` — §0 generalization (sonar target selection, clickable distance
  label, star scope per decision #2); quest-completion check after the existing found
  write-through.
- `include/gpx/gpx_index.h` / `src/gpx/gpx_index.cpp` — `IndexFile`/`IndexEntry` extensions,
  quest query helpers, NVS load/save for quest found-hashes.
- `src/gpx/gpx_loader.cpp` — extend `buildFileIndex()`'s fast scan for `<quest:group>` + `<name>`
  hashing; wire persistence into `markWaypointFound()`.
- `src/gpx/gpx_server.cpp` — upload param handling, `<extensions>` injection, `/list` response
  fields, `UPLOAD_HTML` quest name field + badge/progress display.

## Documentation (per this project's standards)

New subsystem → CHANGELOG.md entry, a `docs/quests.md` component doc, an ADR for the
tagging-mechanism decision (extensions tag vs. filename convention vs. separate manifest — a
real alternative was rejected) and likely a second ADR for the fix/sonar generalization (a real
behavior change to an existing, previously-deliberate design), and a ROADMAP.md status update
once built.

## Verification

- `pio run` clean build.
- Hand-author a synthetic quest GPX (multiple `<wpt>` + the extensions tag), respecting the
  parser's one-element-per-line assumption (a prior test-file generator violated this during the
  two-tier index verification pass — same constraint applies here).
- Upload via the web manager; confirm `/list` shows the quest badge and 0/N progress.
- On hardware: verify the generalized proximity behavior first (§0) independent of quests — walk
  near a non-quest waypoint, confirm sonar+tap-confirm work without fixing it. Then tap each
  quest waypoint in range, confirm `found` sets and persists through the existing
  `gpx index list`-style diagnostic command; confirm completion feedback fires on the last one;
  reboot the device and confirm progress is still there.
