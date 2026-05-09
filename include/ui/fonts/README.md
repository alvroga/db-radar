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

None yet! Start by converting your first font.

**Recommendations**:
- Roboto 16px (body text)
- Roboto 20px (headers)
- Roboto Mono 16px (coordinates)

