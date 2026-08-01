# ADR-0007: Dual panel framebuffers with zero-copy flush

Status: Accepted
Date: 2026-07-28
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

This is distinct from ADR-0006: that ADR is about LVGL's *draw* buffers (where LVGL renders pixels);
this one is about the RGB *panel's* own framebuffers (where the display DMA scans from). With a
single panel framebuffer, every flush had to `memcpy` LVGL's completed draw buffer into it — measured
at 34.0ms, moving 460KB PSRAM→PSRAM at an optimized-`memcpy` rate of ~27MB/s
(`docs/performance_optimization_backlog.md` §2.3/C7). By this point in the 2026-07-28 session, that
flush was already competing with the tiled transpose (ADR-0004) for PSRAM bandwidth — the two were
shown to interact: with the flush still present, a 64-pixel transpose tile beat 32 by 3.4ms; once the
flush was removed, the difference dropped to 0.5ms noise (`CHANGELOG.md:579-582`), meaning the
`memcpy` itself, not just its own cost, was distorting an unrelated tuning decision.

## Decision

Allocate two panel framebuffers (`cfg.num_fbs = 2`, `src/core/device_manager.cpp:469`). Have
`rotate90_tiled` write the rotated frame directly into the *back* framebuffer instead of into a
separate LVGL-owned staging area, and let `esp_lcd_panel_draw_bitmap` recognize when the bitmap
pointer it's handed is already one of its own framebuffers — in which case it swaps `cur_fb_index`
instead of copying (`esp_lcd_panel_rgb.c:614-624`). Shipped in commit `311ca3c` (2026-07-28). Measured:
flush 34.0 → 0.02ms — "deleted, not reduced" (`docs/performance_optimization_backlog.md` §2.3/C7).

## Consequences

**Easier**: the flush step is effectively free, and removing it also *helped* an unrelated stage —
rotation dropped a further 55.7 → 47.4ms once it stopped contending with the flush `memcpy` for
PSRAM bandwidth and cache (`CHANGELOG.md:579-582`). Frame time overall: 149.6 → 94–101ms.

**Harder**: correctness now depends on the driver's own scan-out state, not just on LVGL's buffer
bookkeeping — the transpose must never write into the framebuffer currently being scanned to the
panel. See ADR-0008's `on_frame_buf_complete` guard, added specifically to make that safe.

**Gave up**: PSRAM is more expensive: +460KB for the second panel framebuffer. This is offset, not
free — removing the now-redundant rotation staging buffers that predated this change removed 920KB
net, so the change was PSRAM-*negative* overall (−460KB net) despite the new framebuffer
(`CHANGELOG.md:585-586`). `full_refresh = 1` also became a hard requirement rather than a performance
choice: a partial flush area would leave the rest of the alternate framebuffer holding a two-frames-old
image. See ADR-0008.
