# Custom Fonts Directory

**Place your custom LVGL font files here**

## Quick Start

1. Convert your font at https://lvgl.io/tools/fontconverter
2. Download the `.c` file
3. Place it in this folder
4. Add `LV_FONT_DECLARE(your_font_name);` to `custom_fonts.h`
5. Rebuild the project

## Example Structure

```
include/ui/fonts/
├── README.md            (this file)
├── custom_fonts.h       (font declarations)
├── roboto_14.c          (your converted fonts)
├── roboto_16.c
├── roboto_20.c
└── roboto_mono_16.c
```

## Documentation

See full guide: `docs/custom_fonts.md`

## Current Fonts

- `iosevka_14.c` (14px)
- `iosevka_16.c` (16px) — the most-used size in the current UI
- `iosevka_20.c` (20px)

Each has its own wrapper file in `src/ui/fonts/` (`iosevka_NN_wrapper.c`) and is declared
in `custom_fonts.h`. See `docs/custom_fonts.md` for the full conversion/build process.

