# Display Integration Guide - ST7701 Controller

## Overview

The ST7701 is a 480×480 IPS LCD controller that requires careful timing configuration to avoid display jitter and ensure stable operation.

## Critical Timing Configuration

### **Stable Baseline (Jitter-Free)**
```cpp
// ST7701 RGB Panel Configuration
cfg.timings.h_res = 480;
cfg.timings.v_res = 480;
cfg.timings.pclk_hz = 10000000;  // 10MHz - reduced from 12MHz for stability

// HSYNC Timing (Critical for jitter-free operation)
cfg.timings.hsync_pulse_width = 8;
cfg.timings.hsync_back_porch = 20;   // Increased from 16
cfg.timings.hsync_front_porch = 20;  // Increased from 16

// VSYNC Timing
cfg.timings.vsync_pulse_width = 4;
cfg.timings.vsync_back_porch = 8;
cfg.timings.vsync_front_porch = 10;  // Increased from 8

// Signal Polarities (match vendor specifications)
cfg.timings.flags.hsync_idle_low = 0;
cfg.timings.flags.vsync_idle_low = 0;
cfg.timings.flags.de_idle_high = 0;
cfg.timings.flags.pclk_active_neg = 0;
cfg.timings.flags.pclk_idle_high = 0;
```

### **Performance Optimizations**
```cpp
// PSRAM Alignment for best performance
cfg.psram_trans_align = 64;

// Full-frame LVGL buffer (480 lines = 1 flush/frame, eliminates screen wipe artifact)
// A 10-line SRAM bounce buffer IS configured and active (BOUNCE_BUFFER_LINES = 10,
// system_config.h) — this ESP-IDF version supports it despite an older, incorrect
// note elsewhere claiming otherwise.
cfg.bounce_buffer_size_px = 10 * 480; // ~10 lines, ~18.75KB

// Framebuffer in PSRAM
cfg.flags.fb_in_psram = 1;
```

## Hardware Connections

### **RGB Data Interface (16-bit)**
```cpp
static const int DATA_PINS[16] = {
    5, 45, 48, 47, 21,        // R4-R0
    14, 13, 12, 11, 10, 9,    // G5-G0
    46, 3, 8, 18, 17          // B4-B0
};
```

### **Control Signals**
```cpp
#define LCD_PCLK      41  // Pixel clock
#define LCD_DE        40  // Data enable
#define LCD_VSYNC     39  // Vertical sync
#define LCD_HSYNC     38  // Horizontal sync
```

### **Command Interface (SPI)**
```cpp
#define LCD_MOSI_PIN   1  // SPI MOSI
#define LCD_CLK_PIN    2  // SPI Clock
// LCD_CS controlled via IO expander (software CS)
```

## Rotation — Tiled Transpose, Not `sw_rotate`

The panel is physically mounted 90° CCW in the enclosure; software must compensate 90° CW. **The
default rotation path is a tiled transpose performed inside the flush callback
(`rotate90_tiled`, `src/core/device_manager.cpp`), not LVGL's built-in `sw_rotate`.** This is a
deliberate rewrite, not the original approach — see [ADR-0004](adr/0004-tiled-transpose-display-rotation.md).

```cpp
disp_drv.sw_rotate = 0;              // OFF in the default (TILED) mode
disp_drv.rotated   = LV_DISP_ROT_90; // still set — drives the pixel-order transpose
disp_drv.full_refresh = 1;           // REQUIRED in TILED mode — see below
```

**Why not `sw_rotate`**: LVGL's built-in software rotation works but was measurably slower than a
transpose written for this project's specific memory layout (PSRAM framebuffer, SRAM scratch tile).
The transpose scatters into a small internal-SRAM tile so both PSRAM streams it reads/writes stay
sequential, rather than doing a naive strided copy.

**`full_refresh` is not a free choice in this mode.** The transpose rewrites the *entire* back
framebuffer every flush; a partial-area flush would leave the rest of the frame holding two-frames-old
pixels. `full_refresh` is derived from the active rotation mode at both init and any runtime rotation
switch, and must move with it — see load-bearing constraint #3 in CLAUDE.md's Render Pipeline section.

**Runtime-switchable for comparison/measurement**: `rot on|off|tiled` (serial command) toggles between
the tiled path, LVGL's `sw_rotate`, and no rotation at all — useful for A/B performance checks, not
something to leave off `tiled` in normal operation.

Full measured cost breakdown and the four render-pipeline invariants this interacts with: CLAUDE.md's
Render Pipeline section and [ADR-0008](adr/0008-zero-copy-render-path-invariants.md).

## Initialization Sequence

1. **IO Expander Setup** - Configure LCD_CS control
2. **SPI Command Init** - Send ST7701 initialization commands
3. **RGB Panel Setup** - Configure ESP32 RGB interface
4. **LVGL Integration** - Link with display driver

## Common Issues and Solutions

### **Display Jitter**
- **Cause**: Incorrect HSYNC/VSYNC timing
- **Solution**: Use stable baseline timing above
- **Note**: Reducing PCLK to 10MHz improves stability

### **Color Issues**
- **Cause**: Incorrect RGB pin mapping
- **Solution**: Verify DATA_PINS array matches hardware
- **Test**: Display solid colors to verify mapping

### **SPI Conflicts**
- **Cause**: LCD_CS not properly controlled
- **Solution**: Hold LCD_CS HIGH after initialization
- **Warning**: SD card shares SPI pins - ensure proper CS management

### **Performance Issues**
- **Cause**: Suboptimal memory alignment
- **Solution**: Use 64-byte PSRAM alignment
- **Optimization**: Use full-frame LVGL buffers (480 lines) — eliminates tearing and rotation wipe artifact

## LVGL Integration

```cpp
// Direct framebuffer access
lv_disp_drv_t disp_drv;
lv_disp_drv_init(&disp_drv);
disp_drv.hor_res = 480;
disp_drv.ver_res = 480;
disp_drv.flush_cb = my_disp_flush;
disp_drv.full_refresh = 1;  // Recommended for stability
```

## Debugging Tools

### **Timing Verification**
Monitor serial output for timing confirmation:
```
[RGB] 480x480 pclk=10000000 Hz | H:pw=8 bp=20 fp=20 | V:pw=4 bp=8 fp=10
```

### **Performance Monitoring**
Use diagnostic overlay to monitor FPS and frame timing.

---

*For more advanced display optimization, see the main CLAUDE.md file.*