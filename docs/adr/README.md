# Architecture Decision Records

Started 2026-07-31. An ADR captures *why this option and not the others* for a decision that had
genuine alternatives — not every change, and not "what we built" (that's CHANGELOG.md and the
component docs in `docs/`).

## When to write one

Write an ADR when a future reader could reasonably ask "why didn't they just—". Examples from this
project that would qualify: software vs hardware display rotation, passive vs active BLE scanning,
zero-copy `lv_obj` drawing vs `lv_canvas`, the `I2C_PROCESS_MS = 20` floor. A bug fix, a tuned
constant with one obvious value, or routine feature work does not need one.

## Format

Copy `0000-template.md`, number sequentially (`0001-`, `0002-`, ...), keep it short — Context /
Decision / Consequences, a few sentences each. Link related ADRs and `docs/*.md` pages by relative
path.

**Numbers are stable IDs, assigned in creation order — never renumbered.** When the historical
backfill (see below) eventually runs, its ADRs get appended after whatever number forward-authored
ADRs have reached by then; they are not inserted earlier in the sequence and nothing gets
renumbered to make file order match decision order. Each ADR's `Date:` field carries the real
decision date, so sort on that field for chronological order — not on the filename. This is so a
citation like "ADR-0007" never breaks.

## Status

- **Going forward**: new architectural decisions get an ADR at the time they're made (see CLAUDE.md
  → Documentation Standards).
- **Historical backfill**: decisions made before 2026-07-31 are *not yet* captured as ADRs — they
  live only in CHANGELOG.md, ROADMAP.md, and `docs/performance_optimization_backlog.md`. A plan for
  reconstructing them (a lightweight background pass, not urgent) is prepared in
  [`BACKFILL_PLAN.md`](BACKFILL_PLAN.md).
