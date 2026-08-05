"""
patch_i2c_master_nack_hang.py — PlatformIO extra_script (pre-build)

Backports a single upstream ESP-IDF fix into the vendored, pinned framework
copy of esp_driver_i2c/i2c_master.c.

THE BUG (confirmed by reading the pinned v5.5.0 source directly, 2026-08-02):
s_i2c_send_commands(), on receiving an I2C_EVENT_NACK, waits for the hardware
to finish the STOP condition with:

    while (i2c_ll_is_bus_busy(hal->dev)) {
        __asm__ __volatile__("nop");
    }

This loop has no bound of any kind — it ignores the xfer_timeout_ms the
application passed to i2c_master_transmit()/transmit_receive(). If the bus
never clears after a NACK (a stretched SCL line, or the hardware FSM getting
stuck — both plausible on a marginal/degrading bus), the calling FreeRTOS
task spins here forever. This exactly matches this project's FT-06 freeze
investigation (docs/i2c_bus_freeze_investigation.md, "Occurrence 4"): a
System Task I2C call that never returned, invisible to every failure
counter because nothing downstream of this loop ever ran again.

THE FIX: upstream, Espressif's own issue #17720
(https://github.com/espressif/esp-idf/issues/17720) describes this exact bug
with an identical backtrace, and it was fixed by bounding the loop with the
same timeout already passed in, then calling the existing FSM-reset path on
expiry. Diffed against the fix as it landed in ESP-IDF v5.5.4 (confirmed via
raw.githubusercontent.com) — this project is pinned to framework-espidf
3.50500.0 (IDF 5.5.0) via PlatformIO's registry, whose newest available IDF
5.5.x build is 3.50503.0 (IDF 5.5.3), still unfixed; the next fixed version
requires jumping to IDF 6.0.x, a major-version migration out of scope for
one driver bug. This script applies just the timeout-bound fix (the exact
upstream replacement block, not the surrounding read-batching refactor that
shipped alongside it in 5.5.4) directly to the pinned file at build time.

Idempotent and safe by construction: applies only if the exact known-bad
block is found untouched; if the framework package is ever upgraded and the
surrounding code no longer matches byte-for-byte, this prints a loud warning
and leaves the file alone rather than guessing at a fuzzy patch.
"""
Import("env")
import os

TARGET_REL = os.path.join("components", "esp_driver_i2c", "i2c_master.c")

OLD_BLOCK = """        } else if (event == I2C_EVENT_NACK) {
            // For i2c nack detected, the i2c transaction not finish.
            // start->address->nack->stop
            // So wait the whole transaction finishes, then quit the function.
            while (i2c_ll_is_bus_busy(hal->dev)) {
                __asm__ __volatile__("nop");
            }
        }
"""

NEW_BLOCK = """        } else if (event == I2C_EVENT_NACK) {
            // For i2c nack detected, the i2c transaction not finish.
            // start->address->nack->stop
            // So wait the whole transaction finishes, then quit the function.
            // PATCHED (scripts/patch_i2c_master_nack_hang.py, 2026-08-02): backport of the
            // upstream fix from ESP-IDF v5.5.4 (github.com/espressif/esp-idf/issues/17720) —
            // this loop used to have no bound at all and could hang the calling task forever
            // if the bus never cleared after a NACK. See docs/i2c_bus_freeze_investigation.md,
            // "Occurrence 4" in this project for the freeze this caused.
            TickType_t start_tick = xTaskGetTickCount();
            const TickType_t timeout_ticks = ticks_to_wait;
            while (i2c_ll_is_bus_busy(hal->dev)) {
                if ((xTaskGetTickCount() - start_tick) > timeout_ticks) {
                    ESP_LOGE(TAG, "I2C bus is still busy but software timeout detected");
                    atomic_store(&i2c_master->status, I2C_STATUS_TIMEOUT);
                    s_i2c_hw_fsm_reset(i2c_master, true);
                    break;
                }
            }
        }
"""


def main():
    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
    if not framework_dir:
        print("[I2C_PATCH] WARNING: framework-espidf package dir not found — skipping patch")
        return

    target_path = os.path.join(framework_dir, TARGET_REL)
    if not os.path.isfile(target_path):
        print("[I2C_PATCH] WARNING: {} not found — skipping patch".format(target_path))
        return

    with open(target_path, "r") as f:
        content = f.read()

    if NEW_BLOCK in content:
        print("[I2C_PATCH] i2c_master.c NACK-hang fix already applied — nothing to do")
        return

    if OLD_BLOCK not in content:
        print("=" * 78)
        print("[I2C_PATCH] WARNING: expected unpatched block not found in i2c_master.c.")
        print("[I2C_PATCH] The framework-espidf package version likely changed since this")
        print("[I2C_PATCH] script was written. THE NACK-HANG FIX WAS NOT APPLIED.")
        print("[I2C_PATCH] See scripts/patch_i2c_master_nack_hang.py and re-check whether")
        print("[I2C_PATCH] the pinned framework-espidf version already contains the fix.")
        print("=" * 78)
        return

    content = content.replace(OLD_BLOCK, NEW_BLOCK, 1)
    with open(target_path, "w") as f:
        f.write(content)

    print("[I2C_PATCH] Applied NACK-hang timeout fix to {}".format(target_path))


main()
