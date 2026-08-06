# ADR-0026: CRT scanline effect built and measured, rejected on brightness

Status: Rejected — reverted, no code shipped
Date: 2026-08-06
Decided by: You (Claude proposed and implemented the experiment, you tested on hardware and rejected it)

## Context

ROADMAP.md's "CRT / 8-bit Display Theme" backlog item had, on 2026-08-04, already picked font-only
(LVGL's `LV_FONT_UNSCII_8/16`) over a per-pixel scanline/darken effect in the render path — but that
choice was made without building either option, on cost reasoning alone. It was revisited today because
the render pipeline's own history (see CLAUDE.md's Render Pipeline section) has repeatedly shown that
estimates for this exact hot path have been wrong — three residual-attribution mistakes in the
performance backlog, and the "ESP-IDF doesn't support bounce buffer" claim that turned out false once
someone actually checked. The scanline route deserved the same treatment: build it, measure it, don't
estimate it.

The specific opportunity: `rotate90_tiled` (`src/core/device_manager.cpp`) already transposes every
pixel of every frame through a 2KB internal-SRAM scratch tile as part of the existing 90° rotation (see
CLAUDE.md's Render Pipeline section, load-bearing constraint context). The scatter half of that pass
emits each destination row as a single `memcpy`. The hypothesis was that darkening alternate output rows
during that same pass — RGB565's standard `(c >> 1) & 0x7BEF` halve-brightness trick, one shift-and-mask
per pixel, no extra memory traffic — would cost close to nothing, since the pixel data is already
resident in the SRAM tile at that point and the loop already runs.

## Decision

Built the experiment behind a runtime-only toggle (`rot scanline on|off`, no NVS persistence, no
settings UI), replacing the `memcpy` with a manual per-pixel darken loop on alternate destination rows
when enabled — otherwise identical to the existing tiled-transpose path. Measured on real hardware, then
**rejected and fully reverted** (`git diff` is clean against the pre-experiment commit; no trace of this
ships).

**Performance verdict: real cost, not free, but small.**

| | scanline off | scanline on | delta |
|---|---|---|---|
| tiled rotate | 38.9 ms | 42.1 ms | +3.2 ms (+8%) |
| frame total | 80.2 ms | 86.2 ms | +6.0 ms (+7.5%) |

The "rides for free" hypothesis was wrong, and wrong for a specific, findable reason: the darkened rows
give up the bulk `memcpy` for a scalar per-pixel store loop. The code comment directly above the scatter
loop explains why that matters — `TILE=32` was chosen specifically so each destination run is "four full
cache lines... where the PSRAM write burst stops being dominated by per-run setup." Half the rows losing
that burst-write shape is exactly the mechanism the tile size was tuned around, not an unrelated cost.
This was flagged as the likely gap before building anything (see the conversation history preceding this
ADR) and the measurement confirmed it — a genuine but modest regression, ~8% on the rotate stage alone,
diluted to ~7.5% of total frame time.

**Actual rejection reason: brightness, not performance.** On-device visual check showed the scanline
effect "looks decent, not very good, but decent" — and drops perceived brightness by roughly 20%, on a
display already run near the floor of outdoor readability. That tradeoff was judged unacceptable
regardless of the render cost. Had the brightness cost been acceptable, +6ms/frame (86.2ms vs. the
existing ~85ms documented baseline) would very likely have been too — it's within the same noise band as
other already-accepted stage-to-stage variance in this pipeline.

## Consequences

**Settled**: the scanline/per-pixel route for the CRT/8-bit theme is dead on its own terms (brightness),
not on an unmeasured cost assumption. Nothing about the *performance* question needs revisiting — it was
answered directly, not modeled.

**Font-only (`LV_FONT_UNSCII_8/16`) is now the only live route** for this backlog item, upgraded from
"first choice" to "the only one left standing." It remains unbuilt; the 2026-08-04 scoping (123
`lv_obj_set_style_text_font()` call sites across 7 screen files, UNSCII's fixed pixel size vs. Iosevka's
14/16/20px sizing) still applies unchanged.

**A brightness-recoverable variant was not explored and is explicitly out of scope for now**: e.g.
compensating darkened rows with a backlight/global brightness bump, or a lighter darken factor than a
straight halve. Not pursued because the whole scanline direction was already the secondary option behind
font-only; reopening it with a compensation scheme would be new scope, not a quick follow-up to this
test.

**Nothing shipped.** `device_manager.cpp`, `device_manager.h`, and `diagnostics.cpp` are byte-identical
to before this experiment — verified via `git diff` before closing this out, not assumed from memory of
the revert steps.
