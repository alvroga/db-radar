#include "utils/boot_guard.h"
#include "core/arduino_compat.h"
#include "esp_attr.h"
#include "esp_system.h"

namespace boot_guard {

static constexpr uint32_t MAGIC = 0xB007C0DE;
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_count;

static void validate() {
    // RTC_NOINIT is garbage after a power-on reset; only trust it with the magic AND a
    // software-restart reset reason.
    if (s_magic != MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        s_magic = MAGIC;
        s_count = 0;
    }
}

unsigned failedBoots() {
    validate();
    return s_count;
}

void markBootOk() {
    validate();
    if (s_count != 0) Serial.printf("[BOOT_GUARD] Boot OK - clearing %u failed-boot count\n", (unsigned)s_count);
    s_count = 0;
}

[[noreturn]] void fatal(const char* reason) {
    validate();
    s_count++;
    Serial.printf("[BOOT_GUARD] FATAL: %s (failed boot %u/%u)\n", reason, (unsigned)s_count,
                  (unsigned)MAX_CONSECUTIVE_REBOOTS);

    if (s_count <= MAX_CONSECUTIVE_REBOOTS) {
        Serial.println("[BOOT_GUARD] Rebooting in 2s to retry...");
        delay(2000);
        esp_restart();
    }

    for (;;) {
        Serial.printf("[BOOT_GUARD] HALTED after %u failed boots: %s. Power-cycle the device.\n",
                      (unsigned)MAX_CONSECUTIVE_REBOOTS, reason);
        delay(5000);
    }
}

}
