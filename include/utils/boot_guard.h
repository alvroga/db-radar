#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

// Bounded recovery for fatal boot-time init failures.
//
// Replaces the old `while (1) { delay(1000); }` halts, which turned any transient init
// failure (one missed I2C ACK, a slow peripheral) into a dead device until the user power
// cycled it. fatal() now reboots up to MAX_CONSECUTIVE_REBOOTS times, then halts for good
// (a genuinely stuck bus needs a power cycle, and rebooting forever would just hide that
// and burn the battery). The counter lives in RTC_NOINIT memory: it survives esp_restart()
// but not a power cycle, so a power cycle always gets a fresh set of retries and no NVS
// (not yet initialised at the earliest halt site) is involved.
namespace boot_guard {

constexpr unsigned MAX_CONSECUTIVE_REBOOTS = 3;

// Never returns: reboots, or halts once the cap is reached.
[[noreturn]] void fatal(const char* reason);

// Call once boot has fully succeeded (after tasks are running). Clears the counter.
void markBootOk();

// Consecutive failed boots so far (0 on a clean/power-cycled start).
unsigned failedBoots();

}

#endif
