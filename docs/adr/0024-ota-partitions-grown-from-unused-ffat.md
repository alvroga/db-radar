# ADR-0024: OTA partitions grown to 4MB by reclaiming unused FFat; GPX storage moving off SD

Status: Accepted, revised same day — see addendum below
Date: 2026-08-06
Decided by: You (Claude proposed, discussed alternatives, you chose 3.5MB, then revised to 4MB)

## Addendum (2026-08-06, same day): 3.5MB → 4MB

The original decision below picked 3.5MB over 3MB with the reasoning "the capacity math showed FFat
remains oversized for GPX at every size considered in this range, so there was no reason to take
less" — but stopped short of 4MB without applying that same reasoning past 3.5MB. That was
inconsistent: nothing about 4MB's FFat remainder (7.69MB, down from 8.69MB) crosses into a real
constraint either. The 8,192-entry waypoint index caps usage before FFat bytes do for this project's
own lean GPX format regardless of which of these three sizes was picked, and even for
Geocaching.com-density "plus" imports, 7.69MB still holds ~1,040 full-detail caches — comfortably
inside the 800-2,000 range already judged sufficient.

The deciding factor was recoverability, not capacity: **OTA headroom can only be replenished with a
full USB reflash** (see Consequences below — this can't be pushed as an OTA update), while **FFat
headroom can be freed at any time over the web portal** by deleting/not-uploading files, with no
device access required. That asymmetry means OTA headroom is worth more per byte than FFat headroom
whenever both are already oversupplied for their actual use, which they are here.

Growth-rate math also checked whether the 3.5MB→4MB step bought a meaningful amount of runway: the
partition file's own comment history gives two real binary-size data points, ~1.56MB (2026-04-13) →
1.678507MB (2026-08-06), ~118.5KB over ~115 days ≈ 31KB/month. At that rate, 3.5MB's 1.90MB headroom
is ~5.3 years of runway; 4MB's 2.40MB headroom is ~6.7 years. The extra ~1.4 years wasn't judged
individually decisive — it's the *combination* with the reflash-vs-web-portal asymmetry above that
tipped it, since the 4MB step was confirmed to cost nothing on the FFat side that matters.

One more thing checked before committing to this: whether a bigger OTA slot costs anything at
*update* time. `gpx_server.cpp`'s OTA handler calls `esp_ota_begin(update_part,
OTA_WITH_SEQUENTIAL_WRITES, &ota_handle)` — sectors erase incrementally as data streams in, not the
whole partition upfront, so OTA write/erase time tracks binary size (~1.6MB), not slot size. No
penalty from the larger slot on that path either.

**Also identified during this addendum, not yet acted on**: the biggest plausible future FFat
consumer is quest content (`docs/quests_plan.md`, in progress) — descriptions, hints, and any quest
icons. The codebase currently has **zero** `lv_img`/`LV_IMG_DECLARE` usage and no LVGL filesystem
driver registered (grepped clean, 2026-08-06) — the radar UI is 100% procedural draws today, so there
is no existing precedent for whether quest assets get compiled into flash (`lv_img_dsc_t` C arrays,
OTA cost) or loaded from FFat at runtime (filesystem cost, updatable without a reflash). That choice
is still fully open. Recommendation for whenever quests reach that point: route quest content and
icons through FFat the same way GPX already is, not through the binary — it keeps updates
web-portal-deliverable and keeps this same OTA-vs-FFat asymmetry working in the project's favor
instead of working against it.

`partitions/partitions_ota.csv` now reads 2×4MB OTA slots / 7.69MB FFat. Build-verified: Flash 40.0%
(1,678,507 / 4,194,304 bytes), RAM unchanged at 49.3%.

## Context (original, 3.5MB pass)

`partitions_ota.csv` (2×2MB OTA app slots + 11.7MB FFat + 256KB coredump) has been unchanged since
project inception, carried over from one of Arduino IDE's canned partition-scheme presets — never
chosen for this project's actual needs, and never revisited after the ESP-IDF migration.

Two things surfaced when it was actually examined:

1. **OTA headroom was tight and shrinking.** At the time of this ADR the build was 80% of a 2MB slot
   (409KB free), against a project history of consistently *adding* flash with every feature
   (task manager, waypoint index, beacon proximity, WMM declination, tilt compensation, the OTA
   updater itself). Nothing in that history points down.
2. **FFat was completely unused.** A full grep for FAT-on-flash mount calls found exactly one
   `esp_vfs_fat_*_mount()` in the tree, and it mounts `/sdcard` (the physical SD card,
   `device_manager.cpp:573`) — not `ffat`. 11.7MB of flash had never done anything for this project.

Separately, the actual role of the SD card came into question. Every real storage use today —
GPX files (`/sdcard/gpx`) and all logging (`system_logger`, `field_log`, `tilt_bench`, all under
`/sdcard/logs`) — lives on SD, not FFat. But **the enclosure design makes the physical SD card
inaccessible without disassembling the device.** That matters because GPX data is core to the
device's function; a component whose failure mode requires disassembly to recover from is a bad
place to put a hard dependency. The planned waypoint source going forward is the project's own GPX
generator (lean, purpose-built, for both single files and Quests) rather than heavy Geocaching.com
imports — Geocaching.com compatibility remains supported but is explicitly a "plus," not core, and
capacity math (see below) showed it doesn't need SD's scale to stay useful.

Capacity math done as part of this decision (real GPX files in `assets/gpx/`, byte-counted, not
estimated): Geocaching.com exports run **~5-11KB/waypoint** (avg ~7.4KB — full descriptions, hints,
logs, groundspeak extensions); this project's own lean/synthetic format runs **~169B/waypoint**. The
app's own PSRAM waypoint index (`gpx_index::MAX_INDEX_ENTRIES`) caps at **8,192 waypoints total**
regardless of storage space. At either 8.7MB or 9.7MB of FFat, that cap — not flash space — is the
binding constraint for lean, generator-sourced data (tens of thousands of waypoints fit; the index
can't use more than 8,192 anyway); even for real Geocaching.com-density data, 8.7-9.7MB holds
roughly 800-2,000 full-detail caches, which was judged more than sufficient for a capped "plus"
feature.

## Decision (original, 3.5MB pass — superseded by the addendum above, final numbers are 4MB/7.69MB)

Grow both OTA app slots from 2MB to **3.5MB** (reclaimed entirely from FFat's previously-unused
space), leaving FFat at **~8.69MB**. 3.5MB was chosen over 3MB or leaving it at 2MB because the
capacity math showed FFat remains oversized for GPX at every size considered in this range — the
"cost" of taking the larger OTA headroom doesn't bite anywhere, so there was no reason to take less.
4MB+ was not chosen this round, to avoid over-committing FFat before the GPX-to-FFat migration
(tracked separately in ROADMAP.md, not yet implemented) establishes its real footprint.

*(Revised same day — see addendum at the top: that same "no reason to take less" reasoning was found
to apply past 3.5MB too, so the shipped table is 4MB OTA / 7.69MB FFat, not 3.5MB / 8.69MB.)*

Storage role split going forward:
- **FFat becomes primary storage for GPX files** (migration tracked in ROADMAP.md, not yet built).
  Decouples the device's core function from SD's disassembly-required failure mode.
- **SD keeps dev-only logging for now** (`system_logger`, `field_log`, `tilt_bench`). These default
  off in release builds, aren't a production dependency, and are retrievable over the web portal
  without disassembly — low stakes, no reason to migrate them in the same pass.
- **SD stays in the physical design**, justified going forward by a specific future use — offline
  map-tile/imagery caching, which would need capacity (hundreds of MB+) no onboard flash split could
  provide — not by unlimited GPX headroom, which turned out not to need it. No priority currently;
  noted here so the reasoning for keeping SD isn't lost once GPX moves off it.

## Consequences (final numbers, post-addendum: 4MB OTA / 7.69MB FFat)

**Easier**: OTA headroom goes from 409KB (20%) to ~2.4MB (60%) at current binary size (1,678,507
bytes, unchanged by this partition edit) — real room for the project's established growth trend
without another urgent repartition. GPX storage, once migrated, stops depending on a component that
requires opening the enclosure to service.

**Harder / given up**: FFat's usable headroom drops from 11.7MB to 7.69MB — still large relative to
any realistic GPX footprint, but no longer "everything." Geocaching.com-style heavy imports become
capacity-capped (roughly 1,040 full-detail caches at this FFat size, still inside the 800-2,000 range
judged sufficient) rather than effectively unlimited under SD; accepted, since that use was already
framed as a plus feature, not core.

**Open / deferred, not part of this decision**: The GPX-to-FFat migration itself is not yet
implemented. A real, unrelated bug was found while investigating the logging side of this decision —
`field_log` has no teardown path (no `end()`/`stop()` function exists), so turning dev mode off does
not fully stop it once started at boot — tracked in ROADMAP.md, not blocking this partition change
since dev logging stays on SD regardless of this ADR.

**A repartition itself requires a full USB reflash** — it cannot be pushed as an OTA update, since
the running firmware's understanding of partition offsets is baked in at build time and a mismatched
table between old and new firmware is exactly the failure case OTA can't safely cross.
