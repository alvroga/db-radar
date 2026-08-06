# ADR-0024: OTA partitions grown to 3.5MB by reclaiming unused FFat; GPX storage moving off SD

Status: Accepted
Date: 2026-08-06
Decided by: You (Claude proposed, discussed alternatives, you chose 3.5MB)

## Context

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

## Decision

Grow both OTA app slots from 2MB to **3.5MB** (reclaimed entirely from FFat's previously-unused
space), leaving FFat at **~8.69MB**. 3.5MB was chosen over 3MB or leaving it at 2MB because the
capacity math showed FFat remains oversized for GPX at every size considered in this range — the
"cost" of taking the larger OTA headroom doesn't bite anywhere, so there was no reason to take less.
4MB+ was not chosen this round, to avoid over-committing FFat before the GPX-to-FFat migration
(tracked separately in ROADMAP.md, not yet implemented) establishes its real footprint.

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

## Consequences

**Easier**: OTA headroom goes from 409KB (20%) to ~1.9MB (54%) at current binary size (1,678,507
bytes, unchanged by this partition edit) — real room for the project's established growth trend
without another urgent repartition. GPX storage, once migrated, stops depending on a component that
requires opening the enclosure to service.

**Harder / given up**: FFat's usable headroom drops from 11.7MB to 8.69MB — still large relative to
any realistic GPX footprint, but no longer "everything." Geocaching.com-style heavy imports become
capacity-capped (roughly 800-2,000 full-detail caches at this FFat size) rather than effectively
unlimited under SD; accepted, since that use was already framed as a plus feature, not core.

**Open / deferred, not part of this decision**: The GPX-to-FFat migration itself is not yet
implemented. A real, unrelated bug was found while investigating the logging side of this decision —
`field_log` has no teardown path (no `end()`/`stop()` function exists), so turning dev mode off does
not fully stop it once started at boot — tracked in ROADMAP.md, not blocking this partition change
since dev logging stays on SD regardless of this ADR.

**A repartition itself requires a full USB reflash** — it cannot be pushed as an OTA update, since
the running firmware's understanding of partition offsets is baked in at build time and a mismatched
table between old and new firmware is exactly the failure case OTA can't safely cross.
