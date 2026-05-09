#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <stdint.h>
#include <stddef.h>

namespace memory_manager {

// Memory statistics structure
struct MemoryStats {
    uint32_t heap_free;           // Free heap bytes
    uint32_t heap_size;           // Total heap size
    uint32_t heap_min_free;       // Minimum free ever recorded
    uint32_t psram_free;          // Free PSRAM bytes
    uint32_t psram_size;          // Total PSRAM size
    uint32_t psram_min_free;      // Minimum PSRAM free ever recorded
    uint32_t lvgl_mem_used;       // LVGL memory usage
    uint32_t lvgl_mem_max;        // LVGL memory peak usage
    uint32_t dma_free;            // DMA-capable memory free
    uint32_t allocation_count;    // Number of tracked allocations
    uint32_t deallocation_count;  // Number of tracked deallocations
    bool heap_corruption;        // Heap integrity check result
};

// Configuration structure
struct Config {
    bool enable_tracking = true;     // Track allocations/deallocations
    bool enable_periodic_check = true; // Periodic heap integrity checks
    uint32_t check_interval_ms = 5000;  // Integrity check interval
    uint32_t warning_threshold_bytes = 10240; // Low memory warning threshold
    bool log_stats = false;          // Log stats to serial
};

// Initialization
bool init(const Config& config = Config{});

// Memory monitoring
const MemoryStats& getStats();
void updateStats();
bool checkHeapIntegrity();

// Reporting
void logCurrentStats();
void logMemoryInfo();
void generateMemoryReport();

// Utility functions
uint32_t getFreeHeap();
uint32_t getFreePSRAM();
uint32_t getFreeDMA();
bool isLowMemory();

// LVGL integration
void updateLVGLStats();
void* lvgl_malloc_wrapper(size_t size);
void lvgl_free_wrapper(void* ptr);

// Memory pool management (for frequent allocations)
namespace pools {
    // Small object pool (16-256 bytes)
    void* allocSmall(size_t size);
    void freeSmall(void* ptr);

    // String pool (for UI text)
    void* allocString(size_t size);
    void freeString(void* ptr);

    // Statistics
    uint32_t getSmallPoolUsage();
    uint32_t getStringPoolUsage();
    void logPoolStats();

    // Pool management
    bool initPools();
    void cleanupPools();
}

// Cleanup utilities
namespace cleanup {
    // LVGL object cleanup
    void cleanupLVGLObjects();
    void scheduleCleanup(void* obj, void (*cleanup_fn)(void*));

    // Memory leak detection
    void startLeakDetection();
    void stopLeakDetection();
    void reportLeaks();
}

} // namespace memory_manager

#endif // MEMORY_MANAGER_H