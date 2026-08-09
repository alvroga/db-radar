# Custom Fonts Guide - Iosevka for GPS Radar

This document explains how to add and use custom fonts (Iosevka) in the GPS radar project, including the critical steps needed for LVGL 8.4.0 compatibility.

## Table of Contents
1. [Font Overview](#font-overview)
2. [Recommended Font Sizes](#recommended-font-sizes)
3. [Font Conversion Process](#font-conversion-process)
4. [Critical Fix Required](#critical-fix-required)
5. [Using Custom Fonts in Code](#using-custom-fonts-in-code)
6. [LVGL Built-in Symbols](#lvgl-built-in-symbols)
7. [Memory Considerations](#memory-considerations)

---

## Font Overview

**Font**: Iosevka (https://github.com/be5invis/Iosevka)
- Monospace programming font
- Clean, modern appearance
- Excellent readability at small sizes
- Perfect for displaying coordinates, distances, and technical data

**Current Status**:
- ✅ **iosevka_14** - Working (14px, uncompressed, ~28KB)
- ✅ **iosevka_16** - Working (16px, uncompressed, ~32KB) — the most-used size in the current UI
- ✅ **iosevka_20** - Working (20px, uncompressed, ~37KB)
- No 28px font exists in this project today; the walkthrough below still applies if one is ever added.

---

## Recommended Font Sizes

For a 480×480 circular display with GPS radar UI:

### ✅ Essential Sizes

| Size | Use Case | Examples |
|------|----------|----------|
| **14px** | Small labels, status text | `88%`, `GPS: 12 sats` |
| **16px** | Body text, coordinates, primary labels — the most-used size in the shipped UI | `34.1334°N`, `Waypoint Alpha` |
| **20px** | Section headers, larger status text | `2.4 km`, `Heading: 045°` |

### Sizes not currently used

- **28px+** - Not converted; would suit a large single-number display (speed, main status) if one is ever added
- **Bold variants** - Iosevka is already very readable; bold not currently used

---

## Font Conversion Process

### Step 1: Access LVGL Font Converter
Go to: https://lvgl.io/tools/fontconverter

### Step 2: Configure Font Settings

**Critical Settings** (must be exact):

```
Name: iosevka_14            (or iosevka_20, iosevka_28)
Size: 14 px                  (or 20, 28)
Bpp: 4                       (4-bit per pixel for smooth anti-aliasing)
Range: 0x20-0x7F             (ASCII printable characters: space through ~)
Format: LVGL 8               (IMPORTANT: Must match your LVGL version)
Fallback: None
Kerning: Enabled             (optional, improves spacing)
Compression: DISABLED        ⚠️ CRITICAL - Must be OFF for LVGL 8.4.0!
```

**Why No Compression?**
- LVGL 8.4.0 on ESP32-S3 has issues with compressed fonts
- Compressed fonts render as invisible (zero-width glyphs)
- Uncompressed fonts work perfectly
- Trade-off: ~30-40KB per font vs. broken rendering

### Step 3: Upload Source Font
- Upload `Iosevka-Regular.ttf` (or your preferred Iosevka variant)
- Click "Convert"
- Download the generated `.c` file

### Step 4: Save Font File
Save the downloaded file to:
```
include/ui/fonts/iosevka_14.c    (for 14px)
include/ui/fonts/iosevka_20.c    (for 20px)
include/ui/fonts/iosevka_28.c    (for 28px)
```

---

## Critical Fix Required

**⚠️ IMPORTANT**: The LVGL font converter generates code incompatible with LVGL 8.4.0. You **must** manually fix the generated file.

### Problem
The converter adds this line:
```c
.static_bitmap = 0,
```

But LVGL 8.4.0 doesn't have a `static_bitmap` field in the `lv_font_t` structure.

### Solution
**Find and remove this line** from the generated font file:

**Location**: Near the end of the file, in the font descriptor (around line 744)

**Before** (generated code):
```c
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .static_bitmap = 0,          // ← DELETE THIS LINE
    .dsc = &font_dsc,
```

**After** (fixed code):
```c
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,            // ← .static_bitmap line removed
```

**Build Error if Not Fixed**:
```
error: 'lv_font_t' {aka 'const struct _lv_font_t'} has no member named 'static_bitmap'
```

---

## Using Custom Fonts in Code

### Step 1: Give the New Font Its Own Wrapper File

LVGL font files use static variable names that collide if multiple `.c` fonts land in the
same compilation unit — so this project uses **one wrapper file per font size**, not a
single combined file with commented-out includes. The existing wrappers
(`src/ui/fonts/iosevka_14_wrapper.c`, `iosevka_16_wrapper.c`, `iosevka_20_wrapper.c`) are
each one line:

```c
// Iosevka 14px font wrapper
// Separate wrapper to avoid static variable name collisions

#include "../../../include/ui/fonts/iosevka_14.c"
```

For a new size, add a new `iosevka_NN_wrapper.c` file following the same one-line pattern
— don't add it to an existing wrapper.

### Step 2: Declare the Font

`include/ui/fonts/custom_fonts.h` is where fonts are declared with `LV_FONT_DECLARE()`:

```c
LV_FONT_DECLARE(iosevka_14);  // 14px - UI labels, status text (~28KB)
LV_FONT_DECLARE(iosevka_16);  // 16px - Body text, GPS coordinates (~32KB)
LV_FONT_DECLARE(iosevka_20);  // 20px - Section headers, page titles (~37KB)
```

Add a new line here for any new size, then `#include "fonts/custom_fonts.h"` wherever
the font is used.

### Step 3: Use Font in LVGL Objects

```cpp
// Create label with custom font
lv_obj_t* label = lv_label_create(parent);
lv_obj_set_style_text_font(label, &iosevka_14, 0);
lv_label_set_text(label, "40.7128°N 74.0060°W");

// Or set font for a specific state (e.g., pressed)
lv_obj_set_style_text_font(button, &iosevka_20, LV_STATE_PRESSED);
```

### Example: GPS Coordinates Label

```cpp
// GPS coordinates with Iosevka 14px
lv_obj_t* coords_label = lv_label_create(stage);
lv_obj_set_style_text_color(coords_label, lv_color_white(), 0);
lv_obj_set_style_text_font(coords_label, &iosevka_14, 0);
lv_label_set_text_fmt(coords_label, "%.4f°N %.4f°W", lat, lon);
lv_obj_align(coords_label, LV_ALIGN_BOTTOM_MID, 0, -10);
```

### Example: Header/Status Display

```cpp
// Section header with Iosevka 20px (the largest size currently in the project)
lv_obj_t* status_label = lv_label_create(stage);
lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
lv_obj_set_style_text_font(status_label, &iosevka_20, 0);
lv_label_set_text(status_label, "LOCKED");
lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 20, 20);
```

---

## LVGL Built-in Symbols

LVGL includes **FontAwesome symbols** in the built-in Montserrat fonts. These are perfect for icons and status indicators.

### Useful Symbols for GPS Radar

| Symbol | Macro | Use Case |
|--------|-------|----------|
| 📡 | `LV_SYMBOL_GPS` | GPS satellite status |
| 📶 | `LV_SYMBOL_WIFI` | WiFi connectivity |
| 🔵 | `LV_SYMBOL_BLUETOOTH` | Bluetooth status |
| 🔋 | `LV_SYMBOL_BATTERY_FULL` | Battery level (full) |
| 🪫 | `LV_SYMBOL_BATTERY_EMPTY` | Battery level (low) |
| ⚙️ | `LV_SYMBOL_SETTINGS` | Settings icon |
| 🏠 | `LV_SYMBOL_HOME` | Home/origin marker |
| ➕ | `LV_SYMBOL_PLUS` | Zoom in |
| ➖ | `LV_SYMBOL_MINUS` | Zoom out |
| 🔄 | `LV_SYMBOL_REFRESH` | Update/refresh |
| ⬆️ | `LV_SYMBOL_UP` | North arrow |
| ⬇️ | `LV_SYMBOL_DOWN` | South arrow |
| ⬅️ | `LV_SYMBOL_LEFT` | West arrow |
| ➡️ | `LV_SYMBOL_RIGHT` | East arrow |
| ▶️ | `LV_SYMBOL_PLAY` | Start/resume |
| ⏸️ | `LV_SYMBOL_PAUSE` | Pause |
| ⏹️ | `LV_SYMBOL_STOP` | Stop |
| ⚠️ | `LV_SYMBOL_WARNING` | Warning/alert |
| ✓ | `LV_SYMBOL_OK` | Checkmark/success |
| ✗ | `LV_SYMBOL_CLOSE` | Close/cancel |

### Using Symbols

**Single Symbol**:
```cpp
lv_label_set_text(label, LV_SYMBOL_GPS);
```

**Multiple Symbols**:
```cpp
// Symbols are just strings, so you can concatenate them
lv_label_set_text(label, LV_SYMBOL_GPS LV_SYMBOL_WIFI LV_SYMBOL_BLUETOOTH);
```

**Symbol + Text**:
```cpp
// Combine symbol with text (note: no space needed, it's built into the macro)
lv_label_set_text(label, LV_SYMBOL_GPS " 12 sats");
lv_label_set_text(label, LV_SYMBOL_BATTERY_FULL " 88%");
lv_label_set_text(label, LV_SYMBOL_WIFI " Connected");
```

**Dynamic Symbol + Text**:
```cpp
lv_label_set_text_fmt(label, LV_SYMBOL_GPS " %d sats", sat_count);
lv_label_set_text_fmt(label, LV_SYMBOL_BATTERY_FULL " %d%%", battery_pct);
```

### Example: GPS Status with Symbol

```cpp
// GPS status indicator with symbol
lv_obj_t* gps_status = lv_label_create(stage);
lv_obj_set_style_text_font(gps_status, &lv_font_montserrat_14, 0);

if (gps_locked) {
    lv_label_set_text_fmt(gps_status, LV_SYMBOL_GPS " %d sats", sat_count);
    lv_obj_set_style_text_color(gps_status, lv_color_hex(0x00FF00), 0);  // Green
} else {
    lv_label_set_text(gps_status, LV_SYMBOL_GPS " Searching...");
    lv_obj_set_style_text_color(gps_status, lv_color_hex(0xFFAA00), 0);  // Orange
}
```

### Example: Battery with Symbol and Color

```cpp
lv_obj_t* battery_label = lv_label_create(stage);
lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_14, 0);

if (battery_pct > 80) {
    lv_label_set_text_fmt(battery_label, LV_SYMBOL_BATTERY_FULL " %d%%", battery_pct);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0x00FF00), 0);  // Green
} else if (battery_pct > 20) {
    lv_label_set_text_fmt(battery_label, LV_SYMBOL_BATTERY_2 " %d%%", battery_pct);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0xFFAA00), 0);  // Orange
} else {
    lv_label_set_text_fmt(battery_label, LV_SYMBOL_BATTERY_EMPTY " %d%%", battery_pct);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0xFF0000), 0);  // Red
}
```

**Note**: Symbols are already available in the built-in Montserrat fonts - no additional font conversion needed!

---

## Memory Considerations

### Font Memory Usage (Approximate)

**Uncompressed** (required for LVGL 8.4.0):

| Font Size | Memory Usage | Character Count |
|-----------|--------------|-----------------|
| 14px | ~28 KB | 95 chars (0x20-0x7F) |
| 16px | ~32 KB | 95 chars (0x20-0x7F) |
| 20px | ~37 KB | 95 chars (0x20-0x7F) |

**Total for the 3 shipped fonts**: ~97 KB (per `custom_fonts.h`'s own size comments)

### ESP32-S3 Memory Layout

- **Total Flash**: 16 MB
- **App Partition**: 3 MB
- **Available for fonts**: ~200-300 KB (plenty of room)
- **PSRAM**: 8 MB (not used for font storage, only runtime rendering)

### Memory Optimization Tips

1. **Only include fonts you actually use** - Don't add a wrapper file for a size nothing references
2. **Use symbols from built-in fonts** - Don't convert custom symbol fonts
3. **Limit character ranges** - If you only need numbers, use `0x30-0x39` instead of full ASCII
4. **Share fonts across screens** - Declare fonts globally, don't duplicate

### Example: Numbers-Only Font (Smaller)

If you only need numbers for a large speed display:

```
Range: 0x30-0x39,0x2E    (Numbers 0-9 + decimal point)
Memory saved: ~70% (only 11 chars instead of 95)
```

---

## Troubleshooting

### Font Not Visible
**Symptom**: Text renders with zero width, invisible on screen
**Cause**: Font was converted with compression enabled
**Fix**: Reconvert font with `Compression: DISABLED`

### Build Error: `no member named 'static_bitmap'`
**Symptom**: Compilation fails with error about `static_bitmap`
**Cause**: Generated font file has incompatible field for LVGL 8.4.0
**Fix**: Remove `.static_bitmap = 0,` line from font descriptor

### Multiple Definition Errors
**Symptom**: `redefinition of 'glyph_bitmap'` errors during compilation
**Cause**: Multiple font `.c` files included in the same wrapper file
**Fix**: Give each font size its own wrapper file (`iosevka_NN_wrapper.c`), one `#include` per file

### Font Looks Pixelated
**Symptom**: Font appears jagged or low quality
**Cause**: Wrong Bpp (bit-per-pixel) setting
**Fix**: Use `Bpp: 4` for smooth anti-aliasing

---

## Quick Reference Checklist

When adding a new Iosevka font:

- [ ] Go to https://lvgl.io/tools/fontconverter
- [ ] Set size (14, 20, or 28 px)
- [ ] Set Bpp to **4**
- [ ] Set Range to **0x20-0x7F**
- [ ] Set Format to **LVGL 8**
- [ ] **DISABLE compression** ⚠️
- [ ] Upload Iosevka-Regular.ttf
- [ ] Convert and download
- [ ] Save to `include/ui/fonts/iosevka_XX.c`
- [ ] **Remove `.static_bitmap = 0,` line** ⚠️
- [ ] Create a new `src/ui/fonts/iosevka_XX_wrapper.c` including just that one file
- [ ] Add `LV_FONT_DECLARE(iosevka_XX);` to `include/ui/fonts/custom_fonts.h`
- [ ] Use with `lv_obj_set_style_text_font()`
- [ ] Test on device before committing

---

**Last Updated**: 2026-08-09 (corrected against shipped fonts — 14/16/20px are converted
and in use, not just 14px; build mechanism is one wrapper file per size, not a single
combined file)
**LVGL Version**: 8.4.0
**Platform**: ESP32-S3 (16MB Flash, 8MB PSRAM)
