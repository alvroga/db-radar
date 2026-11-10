#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

// Records why the last boot happened and counts abnormal resets (panic, watchdogs,
// brownout) in NVS namespace "bootinfo". Always on, ~0 RAM; shown on the DEV tab.
namespace boot_info {

// Call once, after NVS is initialised (settings_manager::init()). Idempotent.
void record();

const char* resetReasonName();     // reason for THIS boot
uint32_t bootCount();              // total boots since NVS was last erased
uint32_t crashCount();             // abnormal resets (PANIC/INT_WDT/TASK_WDT/WDT/BROWNOUT)
const char* lastCrashReasonName(); // reason of the most recent abnormal reset, or "none"

}

#endif
