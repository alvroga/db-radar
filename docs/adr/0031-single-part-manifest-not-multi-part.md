# ADR-0031: ESP Web Tools manifest uses one merged-binary part at offset 0, not four separate parts

Status: Accepted
Date: 2026-08-08
Decided by: Claude (proposed), you approved via "Full 3-way setup" scope decision

## Context

ESP Web Tools' manifest format (`web/flasher/manifest.json`) supports listing either a single flash
part or multiple parts, each with its own byte offset. This project's actual flash layout has four
real regions, confirmed from a real build's `.pio/build/db-radar/flasher_args.json`: `bootloader.bin`
@ 0x0, `partitions.bin` @ 0x8000, `ota_data_initial.bin` @ 0xe000, `firmware.bin` @ 0x10000. Both
manifest shapes were viable — either list all four parts individually, or pre-combine them into one
image via `esptool merge_bin` (already needed anyway for the Option B `esptool.py` download, see
ADR-0030) and list that single merged file at offset 0.

## Decision

Single-part manifest: one entry, `db-radar-esp32s3-full.bin` at offset 0 — the same merged binary
already produced for the downloadable-binary install path, reused as-is.

## Consequences

**Easier**: the manifest and the CI merge step only need to agree on one offset (`0`) instead of
four, eliminating a class of bug where the partition table changes (as it already has once, see
ADR-0024) and someone updates `partitions/partitions_ota.csv` + the merge command but forgets the
manifest, or vice versa. The web flasher and the `esptool.py` download path use byte-for-byte the same
artifact, so there's only one binary to reason about, not two representations of the same firmware.

**Harder / given up**: a multi-part manifest's real advantage — letting ESP Web Tools update only the
app partition without re-flashing bootloader/partition-table/otadata on every install — isn't
available here. This project doesn't need it: the web flasher's job is first-time/recovery flashing,
where "Erase device and install" is already the recommended default regardless of part count, and the
steady-state update path (updating an already-flashed board) goes through the existing `/update` OTA
web endpoint (`ota_upload_handler()` in `src/gpx/gpx_server.cpp`), which only ever touches the app
partition already. Nothing in this project's actual usage pattern wants a partial-partition browser
update, so the capability being given up was never going to be exercised.
