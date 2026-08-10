# Documentation Standards — Complete Guide

This is the full version of the process summarized in CLAUDE.md's Documentation Standards section. Read that first for the quick checklist; this page is for the reasoning behind it and the edge cases.

## Why this many documents

Four kinds of writing, each answering a different question, deliberately not merged into one file:

| Document | Answers |
|---|---|
| `CHANGELOG.md` | What changed, and when? (append-only history) |
| `docs/*.md` component docs | How does this subsystem work, right now? (kept current, rewritten in place as the subsystem evolves) |
| `docs/adr/NNNN-*.md` | Why this option and not the real alternatives? (frozen at decision time, never rewritten — see below) |
| `ROADMAP.md` | What's planned/open, one paragraph each, linking to the above for detail |
| `CLAUDE.md` | Fast orientation + currently load-bearing constraints only |

The failure mode this avoids: a single wall of prose that mixes "what we built," "why," and "what's still open" ages badly, because the three decay at different rates. History never changes. Current-state docs need rewriting as the code moves. Open questions resolve and should move to History. Keeping them in separate files means an update to one doesn't require re-deriving the others.

## Component docs (`docs/*.md`)

**Rewritten in place, not append-only.** Unlike CHANGELOG.md or an ADR, a component doc describes *current* behavior — when the subsystem changes, the doc changes with it rather than growing a history section. If you need to know what changed and when, that's what CHANGELOG.md and git blame are for.

**A stale component doc is worse than no doc.** This project has hit this concretely: `docs/display.md` didn't mention the tiled-rotation rewrite and repeated a "no bounce buffer" claim already known to be wrong; `docs/navigation_modes.md` was built entirely around GPS-heading-fusion years after that design was replaced by the compass as sole heading source. Both were caught by cross-checking prose against the actual current code, not by reading the doc in isolation — that's the practical test for "is this doc still trustworthy": open the source file it claims to describe and check.

## Architecture Decision Records (`docs/adr/`)

Full process, index, and numbering rules: [`adr/README.md`](adr/README.md). The short version: an ADR captures *why this option and not the others*, for decisions with genuine alternatives — not every change, and not "what we built" (that's CHANGELOG.md and component docs). Template: [`adr/0000-template.md`](adr/0000-template.md).

**Frozen at decision time — never rewritten**, unlike component docs. If a decision's reasoning later turns out to be wrong or untested, a *new* ADR supersedes it (see ADR-0022 → ADR-0023) or the original ADR gets an explicit caveat appended, not a silent rewrite. The backfilled ADRs (0004-0017) demonstrate this: three of them (0006, 0013, 0014) carry a caveat that their original reasoning is now known to be stale or was never re-verified — that caveat is itself the honest record, not something to clean up.

## CHANGELOG.md

Append-only, chronological, immediate. The Documentation Flow (below) puts it first for a reason: write the one-liner *during* the work, not after — a same-day entry captures details a end-of-day summary loses.

## ROADMAP.md

Summary-only by design — see CLAUDE.md's own note on this. An entry gets a symptom/root cause/status in a few sentences and a link out; if an entry is growing past that, the detail belongs in CHANGELOG.md, not in a longer ROADMAP entry. This project has had entries go stale by drifting out of sync with code that moved on without the ROADMAP entry being updated (see FT-03, FT-05 in ROADMAP.md's Resolved section for two real examples, both closed the same day this was noticed) — the fix in both cases was closing the stale entry against current behavior, not editing it to sound current.

## CLAUDE.md

Fast-orientation file, loaded every session. See its own "CLAUDE.md Size Discipline" note (Documentation Standards section) for the size/audit policy. The operative distinction: CLAUDE.md holds evergreen quick-reference and *currently load-bearing* constraints (things that will silently regress if "cleaned up" by someone who doesn't know why they're there); everything else — feature narrative, measurement history, anything with a component doc already covering it — belongs at the link, not duplicated above it.

## Documentation Flow

1. **During**: one-line CHANGELOG.md entry, immediately
2. **After**: expand with technical details, build impact, user benefits
3. **Component docs**: create/update `docs/*.md` for major features — and re-verify against current code, not just append
4. **ADR**: add `docs/adr/NNNN-title.md` if the change was a decision between real alternatives
5. **ROADMAP.md**: move/update the entry's status, summary-only, linking out for detail
6. **CLAUDE.md**: update only if architecture changed (brief summary + link)
7. **README.md**: update the features list if user-visible

## Quick Checklist (after significant work)

- [ ] CHANGELOG.md entry
- [ ] Build impact measured
- [ ] Component doc created/updated (if needed) — and cross-checked against current code, not just prose-edited
- [ ] ADR added (if a real alternative was rejected)
- [ ] ROADMAP.md status updated (summary-only, link to detail)
- [ ] CLAUDE.md updated (if architecture changed)
- [ ] README.md updated (if user-visible)
