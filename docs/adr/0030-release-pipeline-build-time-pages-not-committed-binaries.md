# ADR-0030: Release pipeline builds once, publishes to both GitHub Releases and GitHub Pages — no binaries committed to git

Status: Accepted
Date: 2026-08-08
Decided by: Claude (proposed), you approved via "Full 3-way setup" scope decision

## Context

The project needed a repeatable way to hand out flashable firmware for three install paths: a
browser-based web flasher (ESP Web Tools), a downloadable binary for `esptool.py`, and the existing
`pio run -t upload` source build. The first two need an actual built `.bin` living somewhere with a
stable URL — this repo has no `.github/workflows/` and no release process today.

Two real alternatives were considered and rejected:

- **(a) Commit built binaries into a tracked folder** (e.g. `web/flasher/*.bin`) and point GitHub
  Pages at that branch/folder directly ("Pages from a branch"). Rejected — binary blobs in git
  history grow unboundedly across releases (this repo already treats binary bloat as a real cost
  elsewhere, see the OTA-partition-size ADR-0024 discussion of FFat vs OTA headroom tradeoffs), and a
  human has to remember to rebuild and re-commit the binary every release, with no CI enforcing that
  the committed binary actually matches the source it's supposed to represent.
- **(b) Two separate workflows** — one for GitHub Releases, one for the Pages deploy — mirroring
  socquique/capsule-radar's own `release.yml` + `webflasher.yml` split (verified by reading their
  actual workflow files). Rejected — two independent builds (potentially at different times, e.g. a
  maintainer manually re-running only one of them) can produce two different binaries for the same
  tag: the Release asset and what the web flasher actually installs could silently disagree.

## Decision

One combined workflow (`.github/workflows/release.yml`), triggered by pushing a tag matching `v*`
(plus a `workflow_dispatch` re-run option for an existing tag). A single `pio run -e db-radar` build
produces the merged full-flash binary and the OTA-only app binary once; that same build's output is
both uploaded as GitHub Release assets (`softprops/action-gh-release@v2`) and copied into the GitHub
Pages artifact (`actions/upload-pages-artifact@v3` / `actions/deploy-pages@v4`) in the same job run.
No binary is ever committed to git.

## Consequences

**Easier**: the Release assets and the web flasher install artifact are structurally guaranteed to be
the same bytes — there is no code path where they can drift, because they come from one build, not
two. Repo history stays free of binary bloat. A release is fully reproducible from a tag: `git tag
vYY.MM.## && git push origin vYY.MM.##` is the entire release procedure.

**Harder / given up**: the web flasher only updates on a tagged release, not on every push to `main`
— judged a feature, not a bug, for a hardware-flashing tool (you don't want the public browser
flasher installing untagged work-in-progress firmware). GitHub Pages must be manually set to "Source =
GitHub Actions" once in repo settings before the first deploy will succeed — this could not be
automated from the environment that authored this pipeline (no working `gh` API access at the time).
