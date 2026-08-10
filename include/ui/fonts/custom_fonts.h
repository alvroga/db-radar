#ifndef CUSTOM_FONTS_H
#define CUSTOM_FONTS_H

#include <lvgl.h>

/**
 * Custom Font System for GPS Radar
 *
 * This file declares custom fonts converted using LVGL Font Converter.
 *
 * To add a new font:
 * 1. Go to https://lvgl.io/tools/fontconverter
 * 2. Upload your TTF/OTF font file
 * 3. Configure settings:
 *    - Name: font_name_size (e.g., "roboto_16")
 *    - Size: 14, 16, 20, 24 px (depending on use case)
 *    - Bpp: 4 (good quality/size balance)
 *    - Range: ASCII (0x20-0x7F) or Latin-1 Extended (0x20-0xFF)
 *    - Compression: Fast
 * 4. Download the generated .c file
 * 5. Save to include/ui/fonts/
 * 6. Add LV_FONT_DECLARE() below
 * 7. Include this header where you need the font
 *
 * Usage example:
 *   #include "fonts/custom_fonts.h"
 *   lv_obj_set_style_text_font(label, &roboto_16, 0);
 */

// ============================================================================
// CUSTOM FONT DECLARATIONS
// ============================================================================

// Iosevka - Programming font optimized for clarity and technical displays
// Perfect for GPS coordinates, numeric data, and technical UI elements
// Character range: ASCII (0x20-0x7F), 4bpp anti-aliasing, compressed
LV_FONT_DECLARE(iosevka_14);  // 14px - UI labels, status text (28KB)
LV_FONT_DECLARE(iosevka_16);  // 16px - Body text, GPS coordinates (32KB)
LV_FONT_DECLARE(iosevka_20);  // 20px - Section headers, page titles (37KB)

// ============================================================================
// FONT CONFIGURATION FOR GPS RADAR
// ============================================================================

/**
 * Recommended fonts for GPS Radar application:
 *
 * 1. ROBOTO or ROBOTO MONO
 *    - Clean, technical appearance
 *    - Great for coordinates and numeric data
 *    - Sizes: 14px (labels), 16px (text), 20px (headers), 24px (titles)
 *
 * 2. INTER
 *    - Modern, highly legible at small sizes
 *    - Excellent for UI labels and status text
 *    - Sizes: 14px (UI labels), 16px (body), 20px (headers)
 *
 * 3. JETBRAINS MONO
 *    - Monospace font perfect for GPS coordinates
 *    - Clear distinction between similar characters (0/O, 1/l)
 *    - Sizes: 14px (coordinates), 16px (numeric data)
 *
 * 4. SF PRO / SAN FRANCISCO
 *    - Apple's system font - clean and modern
 *    - Great for overall UI consistency
 *    - Sizes: 14px, 16px, 20px
 *
 * 5. IBM PLEX SANS
 *    - Technical but friendly appearance
 *    - Good readability on small displays
 *    - Sizes: 14px, 16px, 20px
 */

// ============================================================================
// FONT USAGE GUIDELINES
// ============================================================================

/**
 * Font Size Guidelines for GPS Radar:
 *
 * - 14px: Status labels, small UI elements, secondary information
 * - 16px: Body text, coordinates, primary labels
 * - 20px: Section headers, important status messages
 * - 24px: Page titles, large notifications
 *
 * Memory Considerations:
 * - Each font size generates separate data (~10-30 KB per size)
 * - Current project: 52.3% flash, 36.4% RAM - plenty of room
 * - Recommend 2-4 sizes total to balance flexibility and memory
 *
 * Character Range Recommendations:
 * - ASCII (0x20-0x7F): English only, smallest size (~8 KB)
 * - Latin-1 Extended (0x20-0xFF): European languages (~12 KB)
 * - Full Unicode subset: For multi-language support (varies)
 */

#endif // CUSTOM_FONTS_H
