# ADR-0027: Remove SD→FFat GPX auto-migration entirely

Status: Accepted
Date: 2026-08-07
Decided by: Claude (proposed), you approved via "delete purgeLegacySDGpxFolderOnce and make a new
build without it"

## Context

ADR-0024's same-day addendum shipped `gpx_loader::migrateFromSDIfNeeded()`: a one-time boot-time copy
of any `.gpx` files from the pre-migration SD location (`/sdcard/gpx`) into the new FFat location
(`/ffat/gpx`), gated on "does the FFat gpx folder currently have zero files." That heuristic was true
on the day it was field-verified and stayed true for every ordinary boot afterward — but it could not
distinguish "this device has never migrated" from "the user just deleted every GPX file on purpose."
The original copy also never deleted the SD source, since deleting user data wasn't in scope for a
migration.

Found 2026-08-07: deleting all 18 GPX files (17 waypoints + 1 quest) via the web manager's "select
all → delete all" correctly emptied `/ffat/gpx` (`/list` showed empty, matching real filesystem
state). Rebooting brought every file back, unchanged. A second reboot showed the same files again.
Root cause: the empty FFat folder on reboot #1 was indistinguishable from a fresh device to
`migrateFromSDIfNeeded()`, so it silently re-copied all 18 files back from the still-intact SD
originals. This was corroborated by the reported symptom itself — visual "interference" on the
display recurred multiple times in a row during that reboot, consistent with real flash *write*
operations happening (each file copy), not the read-only behavior a normal reload does. (This
display-interference mechanism — ESP-IDF briefly disabling both cores' cache/interrupts during a raw
SPI-flash program/erase, which FFat sits on — is a plausible, structurally-consistent explanation
but was not directly instrumented; it was not itself the bug being fixed here, and no code changed to
address it.)

## Decision

Two changes were made, in sequence:

1. **First pass**: replaced `migrateFromSDIfNeeded()`'s "is the FFat folder empty" heuristic with
   `purgeLegacySDGpxFolderOnce()` — a check gated on a durable NVS flag (`gpx`/`sd_purged`) instead of
   directory contents, which additionally *deleted* the SD copies (not just ignored them) so no state
   existed anywhere that could resurrect deleted files again. Field-verified: after flashing, the
   already-deleted FFat waypoints stayed empty and did not return.
2. **Second pass, at your explicit request**: removed `purgeLegacySDGpxFolderOnce()` entirely,
   including its NVS flag, the `LEGACY_SD_GPX_FOLDER` constant, and the now-unused `nvs.h` include in
   `gpx_loader.cpp` — rather than leaving a dormant one-time-purge function sitting in the firmware
   permanently. Rationale given: "having something that automatically deletes something is not
   ideal," even gated behind a flag that makes it a no-op after the first run. The one-time purge had
   already done its job on your device before this pass landed, so no functionality was lost by
   removing it.

Two real alternatives were on the table and rejected:

- **(a) Keep the NVS-gated purge as permanent code**, relying on the flag to make every later boot a
  no-op. Rejected per your explicit preference — dormant automatic-delete logic is still
  automatic-delete logic, and its only value (helping devices that hadn't migrated yet) doesn't apply
  once every device in the field has already run it once.
- **(b) Fix the heuristic without deleting the SD source** (e.g., re-derive "already migrated" from
  something other than directory emptiness, but leave old SD files alone as an inert backup).
  Rejected — leaving stale, silently-diverging copies on SD indefinitely is its own footgun (a future
  bug could resurrect them the same way this one did), and you'd already taken your own backup, making
  the SD copies redundant safety margin rather than the only copy.

## Consequences

**Easier**: FFat is now the single, permanent source of truth for GPX files — there is no migration
path in either direction, so "delete a file" means "delete a file," full stop, matching ordinary user
expectation. Nothing in `gpx_loader.cpp` reads or writes `/sdcard/gpx` anymore.

**Harder / given up**: a genuinely fresh device (or one that skipped the brief window when the
NVS-gated purge existed) that still has old files sitting on `/sdcard/gpx` will **not** pick them up
automatically anymore — the user must re-upload via the web portal. ROADMAP.md's first-flash
procedure note about "an old SD card... for the one-time migration to fire" is stale and updated
alongside this ADR.

**Gave up**: any future device that has never run the NVS-gated purge (i.e., was never flashed with
the commit that introduced it) still has the old, buggier `migrateFromSDIfNeeded()`-style resurrection
risk if it happens to run older firmware — not a concern for this project's own hardware (which will
be reflashed with this change), but worth knowing if this template's history is ever bisected.

## Verification status

Field-verified on real hardware for the first pass (NVS-gated purge): deleted files stayed deleted
across two reboots. The second pass (full removal) is a straightforward code deletion with no new
behavior to verify beyond a clean `pio run` build, confirmed.
