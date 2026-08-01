# ADR historical backfill — prepared, not yet run

**Status**: not started. This is a ready-to-fire agent spec for a future session — no need to run it
now. Trigger with something like "run the ADR backfill" and paste the prompt below into a fresh
non-fork agent (a fork would inherit an unrelated conversation; this job wants a clean read of the
repo instead).

## Why this is a backward pass, and why it's low-priority

The project has months of decisions recorded in prose — CHANGELOG.md (2070 lines), ROADMAP.md,
`docs/performance_optimization_backlog.md` (1925 lines), and git history — but not in the ADR format.
Going forward, new decisions get an ADR at the time they're made (see CLAUDE.md → Documentation
Standards), so this backlog stops growing today. Reconstructing the old ones is pure archaeology: it
makes the index faster to scan later but blocks nothing, so it's a good candidate for an overnight /
unattended run with a cheap model rather than something to spend an interactive session on.

## Model choice

Haiku is the right instinct for cost, but this task is closer to *synthesis* (read scattered prose,
identify the actual decision-with-alternatives buried in it, compress to a few sentences) than
extraction, and the source material has a lot of "we tried X, it was wrong, here's why" reasoning
that's easy to summarize into the *wrong* decision if skimmed. Recommend running the survey pass
(step 1 below) on Haiku since it's pure enumeration, but writing the ADR bodies (step 2) with Sonnet.
If cost is the binding constraint, Haiku end-to-end is acceptable — just review harder in step 3.

## Procedure

**Step 1 — survey (produce a candidate list, don't write ADRs yet)**

Read `CHANGELOG.md`, `ROADMAP.md`, `docs/performance_optimization_backlog.md`, every file in `docs/`,
and `git log --oneline` for the full project history. Build a list of candidate decisions — points
where the project chose between real, named alternatives (not bug fixes, not routine feature adds).
Known candidates to confirm/expand, not a closed list:

- Software (LVGL `sw_rotate`) vs hardware display rotation — resolved in favor of software; then a
  *second* decision layered on top: tiled-transpose-in-flush-callback vs LVGL's built-in `sw_rotate`
  (see "Render Pipeline" in CLAUDE.md)
- `lv_canvas` + copy vs zero-copy `lv_obj` draw-event painting for the radar
- Active vs passive BLE scanning for beacon proximity (`docs/beacon_proximity.md`, backlog §7.3d)
- Continuous tempo/mapping vs discrete zone+hysteresis, applied twice (waypoint sonar, beacon ring) —
  arguably one ADR about the *principle*, not two
- `I2C_PROCESS_MS = 20` as a tuned floor, and the 10ms attempt that broke the device
  (`memory/i2c_process_ms_floor.md`, ROADMAP "Sonar Rhythm Defects")
- Compass on shared Wire (GPIO15/7) vs moving to Wire1 (GPIO19/20) — decided to *not* move it
  (`docs/compass_i2c_constraint.md`)
- NimBLE vs Bluedroid BLE stack (~40KB SRAM difference)
- `bounce_buffer` retained despite 18.75KB SRAM cost — open item, not yet decided
  (ROADMAP §1.4/§1.5)
- Body-shadow DF vs BT 5.1 AoA for beacon direction finding — AoA ruled out as hardware-impossible,
  not a preference
- Modular task architecture (FreeRTOS UI/I2C/Network/System tasks) vs the original single-loop design
- Full-frame (480-line) vs partial LVGL buffers — memory/tearing tradeoff

Output of this step: a numbered list, each with a one-line description and a pointer to the source
material (file:line or CHANGELOG date), for a human to skim before any ADR text is written.

**Step 2 — write**

For each confirmed candidate, write `docs/adr/NNNN-title.md` from `0000-template.md`. Keep Context /
Decision / Consequences short — this is a compression exercise, not a rewrite of the source doc. Link
back to the CHANGELOG entry / component doc / backlog section the detail already lives in rather than
duplicating it. **Number continuing from whatever forward-authored ADRs have reached by the time this
runs — check `ls docs/adr/` first, do not start over at `0001` and do not renumber anything that
already exists.** Numbers are stable IDs assigned in creation order, not a chronological index; each
ADR's `Date:` field carries the real historical decision date, so chronological order is recoverable
by sorting on that field instead of the filename (see `docs/adr/README.md`). Set `Decided by: Claude
(backfilled from project history YYYY-MM-DD)` in the header — these are reconstructions, not
decisions made at ADR-writing time, and that should be visible at a glance.

**Step 3 — review**

This is a background/overnight job specifically so a human reviews the output before it's trusted —
flag it back for review rather than committing directly. Spot-check 2-3 ADRs against the source
material for the most likely failure mode: an ADR that states the *fix* as if it were the original
*decision* (e.g. writing "decision: use τ-based EMAs" when the actual decision-with-alternatives was
"continuous mapping vs discrete zones," and the EMA is just an implementation detail of that).
