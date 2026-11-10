#include "utils/boot_info.h"
#include "esp_system.h"
#include "nvs.h"

namespace boot_info {

static const char* NS = "bootinfo";
static bool g_done = false;
static esp_reset_reason_t g_reason = ESP_RST_UNKNOWN;
static uint32_t g_boots = 0, g_crashes = 0;
static uint8_t g_last_crash = 0;

static const char* name(uint8_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case 0:                 return "none";
        default:                return "UNKNOWN";
    }
}

static bool isCrash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

void record() {
    if (g_done) return;
    g_done = true;
    g_reason = esp_reset_reason();

    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u32(h, "boots", &g_boots);
    nvs_get_u32(h, "crashes", &g_crashes);
    nvs_get_u8(h, "last_crash", &g_last_crash);
    g_boots++;
    if (isCrash(g_reason)) {
        g_crashes++;
        g_last_crash = (uint8_t)g_reason;
        nvs_set_u8(h, "last_crash", g_last_crash);
        nvs_set_u32(h, "crashes", g_crashes);
    }
    nvs_set_u32(h, "boots", g_boots);
    nvs_commit(h);
    nvs_close(h);
    printf("[BOOT_INFO] reset=%s boots=%lu crashes=%lu last_crash=%s\n", name(g_reason),
           (unsigned long)g_boots, (unsigned long)g_crashes, name(g_last_crash));
}

const char* resetReasonName() { return name((uint8_t)g_reason); }
uint32_t bootCount() { return g_boots; }
uint32_t crashCount() { return g_crashes; }
const char* lastCrashReasonName() { return name(g_last_crash); }

}
