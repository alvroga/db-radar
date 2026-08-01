# ADR-0010: Continuous passive BLE scan instead of active stop/restart

Status: Accepted
Date: 2026-07-31
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The beacon RSSI feed measured a hard 2.0 Hz against a tag advertising at 5 Hz — roughly 60% of every
advertisement discarded. Backlog §7.3a traced this to three independent limiters stacked in
`beacon_proximity.cpp`: NimBLE's controller-side duplicate filtering (an advertiser is reported to
`onResult` once per scan while it's enabled), `onResult` calling `g_pScan->stop()` on the first hit
each cycle, and a 500ms scan/idle poll loop gating the next `start()`. Separately, reading
`NimBLEScan.cpp` directly (backlog §7.3d) surfaced a correctness issue unrelated to rate: under an
*active* scan, a legacy `ADV_IND` advertiser's `onResult` callback is withheld until its scan response
arrives, or failing that until `BLE_GAP_EVENT_DISC_COMPLETE` — this tag's advertisement type made
active scanning structurally laggy, independent of any polling logic around it.

## Decision

Commit `065cfe1`. Two separable changes, both required: switch to **passive** scanning
(`setActiveScan(false)`) — a correctness fix for this advertiser type, not a power optimization — and
run **one continuous scan** (`start(0, ...)` → `BLE_HS_FOREVER`) with duplicate filtering off
(`setDuplicateFilter(false)`) and `setMaxResults(0)`, removing the stop/restart cycle entirely.
Measured on hardware: 2.0 Hz → 4.24–4.37 Hz (mean gap ~230ms).

## Consequences

**Easier**: the RSSI feed is now fast enough to support work that was previously infeasible — beacon
direction finding (see ADR-0015) was explicitly blocked on this: at 2 Hz a body-rotation scan yielded
only 1.7 samples per 30° bin (noise), while 4–8 Hz yields 4–8 samples per bin (usable). It also made
continuous (rather than zone-quantized) RSSI-driven UI and sonar viable — see ADR-0011.

**Harder**: the scan callback now fires for every advertisement from every nearby device (~30 devices,
69 matching the target MAC in one measured window), not just the target — it must stay allocation-free
(no heap, no `String`; compare a pre-parsed `NimBLEAddress`) or it becomes the new bottleneck. There is
also a standing footgun in the NimBLE API: `setAdvertisedDeviceCallbacks(cb, wantDuplicates)`
internally calls `setDuplicateFilter(!wantDuplicates)`, so calling it *after* the explicit
`setDuplicateFilter(false)` silently restores 2 Hz filtering with nothing logged.
`debugScanAll()` hit this once already (fixed) — see `memory/nimble_scan_footguns.md`.

**Gave up**: nothing — passive scanning also draws less radio power than active, so neither half of
this decision traded against the other.
