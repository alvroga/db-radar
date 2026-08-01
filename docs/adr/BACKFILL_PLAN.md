# ADR historical backfill — ✅ run 2026-08-01

**Status**: complete. Produced ADRs **0004–0017** (14 files), listed in
[`README.md`](README.md#index). The plan below is kept as the record of what was intended; see
[Outcome](#outcome--what-actually-happened) at the end for how it differed in practice.

Everything from here to "Outcome" is the original pre-run spec, unedited.

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

---

## Outcome — what actually happened

Run 2026-08-01. Haiku survey → editorial review → three Sonnet writers in parallel. 19 candidates
surveyed, 14 ADRs written (0004–0017). Four things are worth knowing before anyone reuses this
procedure.

**The model split held up, and the review gate between the steps was where the value was.** Haiku
enumerated and quoted accurately; its *judgement* needed correcting on five of nineteen candidates.
Four were rejected outright as not-decisions — a one-character bug fix (sonar `+=` vs `= now +`), a
tuned constant whose loser stopped mattering rather than losing on merit (32 vs 64px tile), an item
still genuinely undecided (bounce buffer), and `clip_corner`, which the survey itself conceded "was
never a genuine choice" before listing it anyway. Three others (beacon tempo / waypoint tempo /
input-filtering principle) were merged into one ADR, as this plan had already guessed they might be.
Having the survey emit verbatim quotes rather than only pointers is what made that review cheap
enough to do properly, and it meant the writing pass never had to re-read the ~10,300 lines.

**Parallelising the write is safe if — and only if — the numbers are assigned up front.** Three
agents wrote concurrently with fixed number ranges and disjoint file lists. No collisions, no
renumbering.

**The predicted failure mode did occur, in the survey, and was caught.** It quoted the 480-line
buffer's rationale out of CLAUDE.md's section explicitly headed `⚠️ SUPERSEDED` — the one whose own
header says three of its claims are now false — and reproduced a dead justification as current.
Generalise: a backfill agent reading historical prose will not notice that the prose has been
retracted. Anything quoting a doc section marked superseded needs a human check.

**Verification instructions produced better ADRs than the survey could have.** Told to confirm rather
than accept a framing, the writers overturned the survey twice and found material nobody had: 90°
hardware rotation turned out to be *unavailable* on an RGB ST7701 (no line buffer for MADCTL `MV`)
rather than rejected, with two further alternatives — pre-rotating geometry, remounting the panel —
that the backlog explicitly says not to re-propose; and 0006 surfaced a real open gap, that no fresh
justification for `BUFFER_LINES = 480` has been written since the pipeline was rewritten around it.
The lesson is narrow and reusable: point the writing pass at the *specific* claims you doubt.

Three ADRs (0006, 0013, 0014) deliberately record reasoning that is stale or untested rather than
settled — see the caveat in [`README.md`](README.md#status). That is the honest state of the record.
