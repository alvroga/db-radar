# Firmware Installation & Release Pipeline

Maintainer-facing detail behind the README's install options and
[`.github/workflows/release.yml`](../.github/workflows/release.yml). See
[ADR-0030](adr/0030-release-pipeline-build-time-pages-not-committed-binaries.md) and
[ADR-0031](adr/0031-single-part-manifest-not-multi-part.md) for the decisions behind the pipeline
shape; this doc is the "how it actually works and how to verify it still does" reference.

**Scope decision, 2026-08-08**: shipping with only the two binary-distribution paths for now
(browser web flasher + `esptool.py`/Releases). A `pio run -t upload` source-build option is planned
but deliberately held back — the repo isn't public yet, and even once install docs mention only the
two binary paths, a public repo still means GitHub auto-attaches a full source zip to every Release
page (a platform default, not something this pipeline controls). Until the repo is intentionally made
public, treat both binary paths below as **not yet live** — see "Not yet shipped" at the bottom.

## The two install paths, and why each exists

- **Browser web flasher** (`web/flasher/`, deployed to GitHub Pages via ESP Web Tools) — no
  toolchain, works on a genuinely blank board straight from Chrome/Edge over Web Serial. Best for
  first-time setup or recovery when the user doesn't have PlatformIO installed.
- **`esptool.py` + downloaded binary** (GitHub Releases) — same underlying image as the web flasher,
  for users who prefer a CLI or whose browser doesn't support Web Serial.

(`pio run -t upload` from source still exists and is unchanged by this pipeline — it's just not part
of the public install story right now, see the scope decision above.)

Neither of these two touch the device's own OTA update path (`/update`, served by the device itself
once it's running) — that's a separate mechanism for updating an *already-flashed* board over WiFi,
covered below.

## Release pipeline

Triggered by pushing a tag matching `v*` (e.g. `git tag v26.08.50 && git push origin v26.08.50`), or
manually via `workflow_dispatch` against an existing tag. One job:

1. `pio run -e db-radar` — a normal build, no source changes made for CI.
2. Reads the embedded `FW_VERSION` out of the build's own generated
   `include/core/fw_version_gen.h` (see Versioning risk below — this is *not* the same string as the
   git tag that triggered the build).
3. `esptool merge_bin` combines the build's four flash regions into one file,
   `db-radar-esp32s3-full.bin`. **PlatformIO's bundled `tool-esptoolpy` predates `merge_bin`** (pinned
   ~v3.0, `merge_bin` landed ~v3.4) — the workflow installs `esptool` fresh via `pip`, it does not use
   PlatformIO's copy.
4. `firmware.bin` (the build's app image, unmerged) is copied to `db-radar-esp32s3-ota.bin` — no
   transformation, just a rename for clarity in the Release assets.
5. Both binaries are published as GitHub Release assets.
6. The same merged binary, plus `web/flasher/index.html` and `manifest.json` (with the `__FWVER__`
   placeholder substituted for the real version), are deployed to GitHub Pages.

Steps 3–6 all consume the *same* build's output — see ADR-0030 for why that's load-bearing, not
incidental.

### Flash offsets — cross-referenced, not restated

The four regions merged in step 3 come from the project's actual partition table,
[`partitions/partitions_ota.csv`](../partitions/partitions_ota.csv). **If that file's offsets ever
change, the `merge_bin` command in `.github/workflows/release.yml` must change with it** — there is no
mechanism that keeps them in sync automatically. As of this writing (confirmed against a real local
build's `.pio/build/db-radar/flasher_args.json`, not assumed):

| File | Offset | Source |
|---|---|---|
| `bootloader.bin` | `0x0` | ESP-IDF bootloader, not from `partitions_ota.csv` |
| `partitions.bin` | `0x8000` | Compiled form of `partitions/partitions_ota.csv` itself |
| `ota_data_initial.bin` | `0xe000` | Matches the `otadata` row in `partitions_ota.csv` |
| `firmware.bin` | `0x10000` | Matches the `ota_0` row in `partitions_ota.csv` |

Flash settings baked into the merge: `--flash_mode dio --flash_freq 80m --flash_size 16MB` — matches
`board_build.flash_mode`/`f_flash`/`board_upload.flash_size` in `platformio.ini` (see CLAUDE.md's
PlatformIO Settings section for why `dio` here is correct and not a contradiction with the chip
running QIO at runtime).

## Manifest vs. merged binary

`web/flasher/manifest.json` lists a **single** part, `db-radar-esp32s3-full.bin` at offset `0` — the
four regions above are not represented separately in the manifest; they're already combined *inside*
that one file by `merge_bin`. See [ADR-0031](adr/0031-single-part-manifest-not-multi-part.md) for why
a multi-part manifest (listing all four regions individually, which ESP Web Tools also supports) was
rejected.

## Updating an already-flashed board: `/update`, not this pipeline

`ota_upload_handler()` (`src/gpx/gpx_server.cpp`) implements the device's own `/update` web page —
`esp_ota_begin()`/`esp_ota_write()` against `esp_ota_get_next_update_partition()`. It expects the
**raw app image only** — exactly `db-radar-esp32s3-ota.bin`, never `db-radar-esp32s3-full.bin` (which
starts with a bootloader image, not an app image, at its first byte). The release body and the web
flasher page both call this out explicitly to reduce the chance of someone uploading the wrong file.

**Unverified**: whether `esp_ota_end()`'s image validation cleanly rejects an accidental
`-full.bin` upload to `/update`, or does something worse. Expected to reject cleanly (the merged
image's header isn't a valid app-image header), but this has not been tested on real hardware as of
this writing — confirm before asserting it's safe in user-facing docs.

## Versioning: FW_VERSION vs. the git tag

`scripts/gen_version.py` (see [ADR-0025](adr/0025-version-scheme-monthly-build-counter.md)) generates
`FW_VERSION` from a monthly build counter that increments on *every local* build, with no shared
state file — it recovers the counter by re-parsing whatever `fw_version_gen.h` happens to be
committed. Left alone, that counter would drift ahead of whatever tag number a release uses, since
ordinary dev-loop builds (`pio run`, uploads for bench testing) bump it too.

The release workflow closes that gap for anything that actually ships: it sets
`FW_VERSION_OVERRIDE` to the exact tag being built, and the `Verify embedded FW_VERSION matches the
release tag` step **fails the build** if the compiled header doesn't match. So for every tagged
release, the tag and the embedded `FW_VERSION` are identical by construction — not just "usually
close." The drift only exists between releases, in local dev builds that were never tagged (see
"When to cut a release" below for why that's expected and fine).

## When to cut a release

Tag **when `main` sits at a coherent, working checkpoint** — not on every commit, and not on a
calendar cadence. Concretely:

- **Do tag** once a feature or fix that shipped to `main` is either field-verified, or build-clean and
  low-risk enough that field-verification can happen against the released build itself (this project
  has done both). A batch of several such commits landing together (e.g. a new hardware module +
  a couple of bug fixes) is a fine single release — no need for one tag per commit.
- **Don't tag** mid-feature, against WIP still on a branch, or against something you know is broken/
  untested on real hardware. `main` itself should already reflect that bar (see CHANGELOG.md's
  `[Unreleased]` section, which calls out anything merged but not yet field-tested).
- **Before tagging**, confirm CHANGELOG.md's `[Unreleased]` section actually describes what's about
  to ship — that's the fastest sanity check that nothing half-finished is riding along.
- **Tag naming**: use whatever `FW_VERSION` is currently committed in
  `include/core/fw_version_gen.h` (`vYY.MM.##`) — don't invent a separate release-numbering sequence.
  That's what makes the override-verification step above a no-op success instead of a guaranteed
  failure.
- **After tagging**: `git push origin <tag>` triggers the workflow; watch it with
  `gh run watch --exit-status`, then spot-check `gh release view <tag>` and the flasher URL
  (https://alvroga.github.io/db-radar/flasher/) actually reflect the new version.

## One-time manual setup (done)

GitHub Pages is set to **Settings → Pages → Build and deployment → Source = "GitHub Actions"**, and
the repo is public — both required for the install paths above to work, and both already in place
(see CHANGELOG.md's 2026-08-08 pipeline entry and the rename-to-public work around 2026-08-14). Only
worth revisiting if either setting is ever found reverted.
