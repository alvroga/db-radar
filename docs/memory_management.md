# Memory Management System Guide

Real-time heap/PSRAM/LVGL/DMA monitoring, ultra-conservative fixed-size pools, automatic heap
corruption checks, and a serial diagnostic interface. See CLAUDE.md's Memory Management section for
the quick-reference summary — this doc covers the two things worth a full explanation: why the pools
are sized the way they are, and the full command reference.

## Serial Commands

```bash
memory stats               # Current memory statistics (heap, PSRAM, LVGL, DMA)
memory info                # Memory layout information
memory report              # Full memory report (stats + pools + integrity)
memory pools                # Static memory pool usage
memory cleanup [screens|lvgl]  # Force cleanup; no argument = both
memory integrity           # Heap integrity check (PASS/FAIL)
memory leak start|stop|report  # Leak-detection tracking
memory stress               # Pool allocation stress test
```

(`memory_manager.h`/`.cpp`; dispatched from `handleMemoryCommand()` in `src/utils/diagnostics.cpp`.)

## Memory Pool Architecture

```
Small Object Pool: 2 × 256B = 512B   ← Fast allocation for small objects
String Pool:       2 × 128B = 256B   ← Optimized for text/string storage
Total: ~768 bytes (0.2% of available RAM)
```

Static allocation (no `malloc()` during pool creation), boundary-checked, falls back to standard
`malloc()` if a pool is full.

## Why 768 bytes, not something larger

The pools were initially sized much larger (32+16 blocks ≈ 12KB), which caused boot loops — excessive
memory pressure during initialization, before the rest of the system had a chance to settle. Dropping
to the current ultra-conservative sizing (2+2 blocks ≈ 768 bytes) fixed it while still providing the
same pooling behavior for the small/short-lived allocations it targets. If pool sizes are ever
revisited, treat this history as a real constraint, not an arbitrary starting point — retest boot
stability, don't just scale the numbers up.
