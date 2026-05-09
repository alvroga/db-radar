#include "memory_manager.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "core/arduino_compat.h"
#include <lvgl.h>

namespace memory_manager {

// Global state
static Config g_config;
static MemoryStats g_stats = {};
static bool g_initialized = false;
static uint32_t g_last_check = 0;

// Memory pool structures - Phase 2: Static arrays for safety
namespace pools {
    // Simple fixed-size pools using static arrays (safer than malloc)
    struct PoolBlock {
        bool in_use;
        size_t size;
    };

    static const size_t SMALL_POOL_SIZE = 2;    // Ultra-conservative: 2 blocks for testing
    static const size_t STRING_POOL_SIZE = 2;   // Ultra-conservative: 2 blocks for testing
    static const size_t SMALL_BLOCK_SIZE = 256; // Max size for small pool
    static const size_t STRING_BLOCK_SIZE = 128; // Max size for string pool

    // Static arrays - no malloc, crash-safe
    static uint8_t g_small_blocks[SMALL_POOL_SIZE][SMALL_BLOCK_SIZE];
    static uint8_t g_string_blocks[STRING_POOL_SIZE][STRING_BLOCK_SIZE];
    static PoolBlock g_small_pool[SMALL_POOL_SIZE];
    static PoolBlock g_string_pool[STRING_POOL_SIZE];
    static bool g_pools_initialized = false;
}

bool init(const Config& config) {
    // Safety check: prevent double initialization
    if (g_initialized) {
        Serial.println("[MEMORY] Already initialized");
        return true;
    }

    g_config = config;

    // Initialize statistics to zero
    memset(&g_stats, 0, sizeof(g_stats));

    // Initialize memory statistics
    updateStats();

    // Initialize memory pools if tracking is enabled
    if (g_config.enable_tracking) {
        if (!pools::initPools()) {
            Serial.println("[MEMORY] Warning: Failed to initialize memory pools, continuing without pools");
            g_config.enable_tracking = false; // Disable tracking to prevent crashes
        }
    }

    g_initialized = true;
    Serial.println("[MEMORY] Memory manager initialized");

    // Set up LVGL memory hooks if available (after initialization flag is set)
    #if LV_USE_USER_DATA
    updateLVGLStats();
    #endif

    if (g_config.log_stats) {
        logCurrentStats();
    }

    return true;
}

const MemoryStats& getStats() {
    return g_stats;
}

void updateStats() {
    if (!g_initialized) return;

    // Update heap statistics
    g_stats.heap_free = esp_get_free_heap_size();
    g_stats.heap_size = esp_get_minimum_free_heap_size() + g_stats.heap_free;

    // Track minimum free heap
    if (g_stats.heap_min_free == 0 || g_stats.heap_free < g_stats.heap_min_free) {
        g_stats.heap_min_free = g_stats.heap_free;
    }

    // Update PSRAM statistics
    g_stats.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    g_stats.psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    // Track minimum free PSRAM
    if (g_stats.psram_min_free == 0 || g_stats.psram_free < g_stats.psram_min_free) {
        g_stats.psram_min_free = g_stats.psram_free;
    }

    // Update DMA-capable memory
    g_stats.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);

    // Update LVGL memory usage
    #if LV_USE_MEM_MONITOR
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    g_stats.lvgl_mem_used = mon.total_size - mon.free_size;
    if (g_stats.lvgl_mem_used > g_stats.lvgl_mem_max) {
        g_stats.lvgl_mem_max = g_stats.lvgl_mem_used;
    }
    #endif

    // Periodic heap integrity check
    if (g_config.enable_periodic_check) {
        uint32_t now = millis();
        if (now - g_last_check >= g_config.check_interval_ms) {
            g_stats.heap_corruption = !checkHeapIntegrity();
            g_last_check = now;
        }
    }
}

bool checkHeapIntegrity() {
    return heap_caps_check_integrity_all(true);
}

void logCurrentStats() {
    updateStats();

    Serial.println("==== Memory Statistics ====");
    Serial.printf("Heap:     %u / %u bytes (min: %u)\n",
                  g_stats.heap_free, g_stats.heap_size, g_stats.heap_min_free);
    Serial.printf("PSRAM:    %u / %u bytes (min: %u)\n",
                  g_stats.psram_free, g_stats.psram_size, g_stats.psram_min_free);
    Serial.printf("DMA:      %u bytes available\n", g_stats.dma_free);

    #if LV_USE_MEM_MONITOR
    Serial.printf("LVGL:     %u bytes used (max: %u)\n",
                  g_stats.lvgl_mem_used, g_stats.lvgl_mem_max);
    #endif

    Serial.printf("Tracking: alloc=%u, free=%u\n",
                  g_stats.allocation_count, g_stats.deallocation_count);

    if (g_stats.heap_corruption) {
        Serial.println("WARNING: Heap corruption detected!");
    }

    if (isLowMemory()) {
        Serial.println("WARNING: Low memory condition detected!");
    }

    Serial.println("===========================");
}

void logMemoryInfo() {
    Serial.println("==== Memory Layout Info ====");
    Serial.printf("Internal RAM: %u bytes\n", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    Serial.printf("External RAM: %u bytes\n", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    Serial.printf("DMA capable: %u bytes\n", heap_caps_get_total_size(MALLOC_CAP_DMA));
    Serial.printf("32-bit access: %u bytes\n", heap_caps_get_total_size(MALLOC_CAP_32BIT));

    // Largest free blocks
    Serial.printf("Largest internal block: %u bytes\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    Serial.printf("Largest PSRAM block: %u bytes\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    Serial.println("=============================");
}

void generateMemoryReport() {
    updateStats();
    logMemoryInfo();
    logCurrentStats();

    #if LV_USE_PERF_MONITOR
    // LVGL performance info
    Serial.println("==== LVGL Performance ====");
    // Note: LVGL 8.x performance monitoring would be added here
    Serial.println("=========================");
    #endif

    // Memory pool statistics
    if (g_config.enable_tracking) {
        pools::logPoolStats();
    }
}

uint32_t getFreeHeap() {
    return esp_get_free_heap_size();
}

uint32_t getFreePSRAM() {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t getFreeDMA() {
    return heap_caps_get_free_size(MALLOC_CAP_DMA);
}

bool isLowMemory() {
    return (g_stats.heap_free < g_config.warning_threshold_bytes) ||
           (g_stats.psram_free < g_config.warning_threshold_bytes * 4); // PSRAM threshold is higher
}

void updateLVGLStats() {
    #if LV_USE_MEM_MONITOR
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    g_stats.lvgl_mem_used = mon.total_size - mon.free_size;
    if (g_stats.lvgl_mem_used > g_stats.lvgl_mem_max) {
        g_stats.lvgl_mem_max = g_stats.lvgl_mem_used;
    }
    #endif
}

// Memory pool implementations
namespace pools {

bool initPools() {
    // Safety check: prevent double initialization
    if (g_pools_initialized) {
        Serial.println("[MEMORY] Pools already initialized");
        return true;
    }

    // Initialize small object pool - static arrays, no malloc
    memset(g_small_pool, 0, sizeof(g_small_pool));
    memset(g_small_blocks, 0, sizeof(g_small_blocks));

    for (size_t i = 0; i < SMALL_POOL_SIZE; i++) {
        g_small_pool[i].in_use = false;
        g_small_pool[i].size = SMALL_BLOCK_SIZE;
    }

    // Initialize string pool - static arrays, no malloc
    memset(g_string_pool, 0, sizeof(g_string_pool));
    memset(g_string_blocks, 0, sizeof(g_string_blocks));

    for (size_t i = 0; i < STRING_POOL_SIZE; i++) {
        g_string_pool[i].in_use = false;
        g_string_pool[i].size = STRING_BLOCK_SIZE;
    }

    g_pools_initialized = true;
    Serial.printf("[MEMORY] Initialized static pools: %zu small blocks (%zu bytes each), %zu string blocks (%zu bytes each)\n",
                  SMALL_POOL_SIZE, SMALL_BLOCK_SIZE, STRING_POOL_SIZE, STRING_BLOCK_SIZE);
    Serial.printf("[MEMORY] Total pool memory: %zu bytes (static allocation, crash-safe)\n",
                  (SMALL_POOL_SIZE * SMALL_BLOCK_SIZE) + (STRING_POOL_SIZE * STRING_BLOCK_SIZE));
    return true;
}

void* allocSmall(size_t size) {
    if (!g_initialized || !g_pools_initialized || size > SMALL_BLOCK_SIZE) {
        return malloc(size); // Fall back to regular malloc
    }

    for (size_t i = 0; i < SMALL_POOL_SIZE; i++) {
        if (!g_small_pool[i].in_use) {
            g_small_pool[i].in_use = true;
            if (g_initialized) g_stats.allocation_count++;
            return g_small_blocks[i]; // Return pointer to static block
        }
    }

    // Pool exhausted, fall back to malloc
    return malloc(size);
}

void freeSmall(void* ptr) {
    if (!g_initialized || !g_pools_initialized || !ptr) {
        if (ptr) free(ptr); // Safe fallback
        return;
    }

    // Check if pointer belongs to our static pool
    for (size_t i = 0; i < SMALL_POOL_SIZE; i++) {
        if (g_small_blocks[i] == ptr) {
            g_small_pool[i].in_use = false;
            if (g_initialized) g_stats.deallocation_count++;
            return;
        }
    }

    // Not from pool, use regular free
    free(ptr);
}

void* allocString(size_t size) {
    if (!g_initialized || !g_pools_initialized || size > STRING_BLOCK_SIZE) {
        return malloc(size); // Fall back to regular malloc
    }

    for (size_t i = 0; i < STRING_POOL_SIZE; i++) {
        if (!g_string_pool[i].in_use) {
            g_string_pool[i].in_use = true;
            if (g_initialized) g_stats.allocation_count++;
            return g_string_blocks[i]; // Return pointer to static block
        }
    }

    // Pool exhausted, fall back to malloc
    return malloc(size);
}

void freeString(void* ptr) {
    if (!g_initialized || !g_pools_initialized || !ptr) {
        if (ptr) free(ptr); // Safe fallback
        return;
    }

    // Check if pointer belongs to our static pool
    for (size_t i = 0; i < STRING_POOL_SIZE; i++) {
        if (g_string_blocks[i] == ptr) {
            g_string_pool[i].in_use = false;
            if (g_initialized) g_stats.deallocation_count++;
            return;
        }
    }

    // Not from pool, use regular free
    free(ptr);
}

uint32_t getSmallPoolUsage() {
    if (!g_initialized || !g_pools_initialized) return 0;

    uint32_t used = 0;
    for (size_t i = 0; i < SMALL_POOL_SIZE; i++) {
        if (g_small_pool[i].in_use) used++;
    }
    return used;
}

uint32_t getStringPoolUsage() {
    if (!g_initialized || !g_pools_initialized) return 0;

    uint32_t used = 0;
    for (size_t i = 0; i < STRING_POOL_SIZE; i++) {
        if (g_string_pool[i].in_use) used++;
    }
    return used;
}

void logPoolStats() {
    Serial.println("==== Memory Pool Statistics ====");
    Serial.printf("Small pool:  %u / %zu blocks used\n",
                  getSmallPoolUsage(), SMALL_POOL_SIZE);
    Serial.printf("String pool: %u / %zu blocks used\n",
                  getStringPoolUsage(), STRING_POOL_SIZE);
    Serial.println("================================");
}

void cleanupPools() {
    if (g_pools_initialized) {
        // Reset all pool blocks to available state
        for (size_t i = 0; i < SMALL_POOL_SIZE; i++) {
            g_small_pool[i].in_use = false;
        }
        for (size_t i = 0; i < STRING_POOL_SIZE; i++) {
            g_string_pool[i].in_use = false;
        }

        // Clear static arrays (optional, for security)
        memset(g_small_blocks, 0, sizeof(g_small_blocks));
        memset(g_string_blocks, 0, sizeof(g_string_blocks));

        g_pools_initialized = false;
        Serial.println("[MEMORY] Static pools cleaned up");
    }
}

} // namespace pools

// Cleanup utilities
namespace cleanup {

// Simple cleanup scheduler for deferred object cleanup
struct CleanupItem {
    void* obj;
    void (*cleanup_fn)(void*);
    CleanupItem* next;
};

static CleanupItem* g_cleanup_list = nullptr;

void cleanupLVGLObjects() {
    // This function can be called to clean up any dangling LVGL objects
    // LVGL 8.x has automatic garbage collection, but we can help it

    // Update LVGL memory stats before cleanup
    updateLVGLStats();
    uint32_t mem_before = g_stats.lvgl_mem_used;

    // Process any pending LVGL cleanup tasks
    lv_timer_handler();

    // Cleanup any orphaned objects by walking the object tree
    lv_obj_t* scr = lv_scr_act();
    if (scr) {
        // LVGL will automatically clean up when objects are deleted
        // We just need to ensure we're not holding dangling pointers

        // LVGL 8.x doesn't have lv_style_reset_cache, but we can force a refresh
        // by calling lv_obj_report_style_change on all objects
        lv_obj_report_style_change(NULL); // NULL means all objects
    }

    // Update stats after cleanup
    updateLVGLStats();
    uint32_t mem_after = g_stats.lvgl_mem_used;

    Serial.printf("[MEMORY] LVGL cleanup: %u -> %u bytes (freed: %d)\n",
                  mem_before, mem_after, (int)(mem_before - mem_after));
}

void scheduleCleanup(void* obj, void (*cleanup_fn)(void*)) {
    if (!obj || !cleanup_fn) return;

    CleanupItem* item = (CleanupItem*)malloc(sizeof(CleanupItem));
    if (!item) return;

    item->obj = obj;
    item->cleanup_fn = cleanup_fn;
    item->next = g_cleanup_list;
    g_cleanup_list = item;
}

void startLeakDetection() {
    // Reset tracking counters
    g_stats.allocation_count = 0;
    g_stats.deallocation_count = 0;
    Serial.println("[MEMORY] Leak detection started");
}

void stopLeakDetection() {
    Serial.println("[MEMORY] Leak detection stopped");
}

void reportLeaks() {
    uint32_t potential_leaks = g_stats.allocation_count - g_stats.deallocation_count;
    if (potential_leaks > 0) {
        Serial.printf("[MEMORY] Potential memory leaks: %u allocations not freed\n", potential_leaks);
    } else {
        Serial.println("[MEMORY] No memory leaks detected in tracking period");
    }
}

} // namespace cleanup

// LVGL integration functions
void* lvgl_malloc_wrapper(size_t size) {
    if (!g_initialized) {
        return malloc(size);
    }

    // For small LVGL objects, try pool allocation first
    if (size <= 256) {
        void* ptr = pools::allocSmall(size);
        if (ptr) {
            return ptr;
        }
    }

    // Fall back to regular malloc
    void* ptr = malloc(size);
    if (ptr && g_initialized) {
        g_stats.allocation_count++;
    }
    return ptr;
}

void lvgl_free_wrapper(void* ptr) {
    if (!ptr || !g_initialized) {
        if (ptr) free(ptr);
        return;
    }

    // Try pool deallocation first
    if (pools::getSmallPoolUsage() > 0) {
        // Check if this pointer belongs to our pools
        pools::freeSmall(ptr); // This will handle fallback to free() if not from pool
    } else {
        free(ptr);
    }

    if (g_initialized) {
        g_stats.deallocation_count++;
    }
}

} // namespace memory_manager