# ADR-0028: GPX upload/delete no longer auto-reload; client calls /reload once per batch

Status: Accepted
Date: 2026-08-07
Decided by: Claude (proposed), you approved via "yes, let's do... the display corruption fix and
the indexing"

## Context

Stress-testing the `MAX_INDEX_FILES = 512` raise (see ADR's sibling context in
`docs/quests_plan.md` §0.5) surfaced a real scaling problem, not just a theoretical one: uploading
100 files took 1m22s; the next 100 (same size batch) took 3m36s — more than 2.6x slower for an
equal-sized batch, confirming non-linear growth rather than a fixed per-file cost.

Root cause: `upload_handler()` and `delete_handler()` (`gpx_server.cpp`) each called
`gpx_loader::refreshGPXFiles()` — a full reload: reset the PSRAM index, `readdir()` the entire
folder, re-run `buildFileIndex()` (a full byte-by-byte scan) on *every* file currently present, then
re-run closest-N selection and re-materialize the working set — after every single request. For a
batch of N files, request *i* rescans all *i* files uploaded/kept so far, making total batch work
O(N²). The browser's `handleFiles()`/`deleteSelected()` compounded this by also calling `/list`
(which itself reads every file's cached name via `extractGpxName()`) after every single file.

Boot is unaffected by this — `main.cpp` calls `loadAllGPXFiles()` exactly once, so it was always
O(N), never O(N²); this ADR only concerns the interactive upload/delete path.

## Decision

Removed the automatic reload from both `upload_handler()` and `delete_handler()` entirely. Added a
new `POST /reload` endpoint that does exactly what the removed code did (rebuild index, re-select,
queue a radar refresh). The client (`handleFiles()`, `deleteSelected()`, `deleteFile()`) now calls
`/reload` itself, once, after all files in a given operation have been sent — not once per file —
and moved the post-operation `loadFileList()`/`loadWaypointCount()` calls out of the per-file loops
to run once as well. This converts total batch cost from O(N²) to O(N): each file still costs one
real upload/delete request (irreducible — each is a distinct FFat write/erase), but the index
rebuild and file-list refresh now happen once per *batch*, not once per *file*.

Two real alternatives were considered and rejected:

- **(a) Keep auto-reload, add an opt-in `?defer=1` query param** that the client sets on all but the
  last request in a batch. Rejected — every caller has to remember to pass the flag correctly (get
  it wrong and you're back to O(N²) silently), and the "last request does the real work" special
  case adds branching for no real benefit over just always deferring.
- **(b) A true multipart batch endpoint** (accept N files in one HTTP request). Rejected as
  disproportionate — it would still be N separate real FFat writes underneath (each file still needs
  its own `fopen`/`fwrite`/`fclose`), so it wouldn't remove any more actual work than this decision
  does; it would only save N−1 HTTP round trips, which weren't the bottleneck (the O(N²) reload cost
  was, and is fixed either way). Not ruled out for later if round-trip count itself ever becomes the
  bottleneck.

## Consequences

**Easier**: batch upload/delete scales linearly instead of quadratically. Normal single-file
usage is functionally unchanged (one upload/delete request, then one `/reload` request — same total
server-side work as the old single-request design, just split across two round trips instead of
one).

**Harder / given up**: `/upload` and `/delete/<filename>` are no longer self-consistent as HTTP
endpoints — a caller that hits them directly (curl, a future script, anything other than this
project's own web UI) without also calling `/reload` afterward will have the file genuinely written
to or removed from FFat, but the live radar/waypoint index won't reflect it until something else
triggers a reload (another `/reload` call, or a reboot). This is a real behavior change from before,
where every request was self-contained. There is no other consumer of these endpoints today besides
this project's own web UI, so it's not breaking anything live, but it's a contract a future
integration needs to know about.

**Gave up nothing on correctness in the gap between a delete and the next `/reload`**: the PSRAM
index can briefly hold `file_offset`s into a just-deleted file. This was already handled gracefully
before this change — `gpx_loader::reselect()`/`selectAndMaterialize()` treat a failed `fopen()` on a
stale entry as "drop this one from the working set," not a crash — so widening that window doesn't
introduce a new failure mode, only makes an already-safe path slightly more likely to be exercised.

## Verification status

**Field-verified 2026-08-07.** Completed the remaining ~312 files of the original 512-file stress
test with this change in place: an equal-sized 100-file batch that previously took 3m36s took 24s
— roughly 9x faster, consistent with the O(N²)→O(N) fix. All 512 files uploaded successfully.
Radar boot/render time was unaffected by the much larger file/waypoint count ("takes almost the
exact same time it used to").

**The `CONFIG_SPI_FLASH_AUTO_SUSPEND` flash-suspend change (also shipped alongside this) does not
appear to have resolved the display corruption** — inferred from the fact that, after this test,
the request was to add a persistent UI warning that uploads/deletes cause visible interference
(see the web UI's added "Note" box), rather than to continue chasing the root cause. Flash-suspend
may still be reducing frequency or severity to some degree; this wasn't isolated or measured
separately from the O(N²) fix in the same test, so no claim is made either way. The corruption is
now being treated as accepted, expected behavior (documented in the UI) rather than something to
keep pursuing.
