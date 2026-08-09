"""
patch_nimble_cpp_idf5.py — PlatformIO extra_script (pre-build)

Two compile-breaking bugs in esp-nimble-cpp v1.4.1 (this project's pinned
version, platformio.ini's lib_deps) when built against ESP-IDF 5.x. v1.4.x
was written against IDF 4.x/5.0; these were never fixed upstream on the
v1.4.1 tag.

FOUND THE HARD WAY (2026-08-09): these two patches previously existed only
as hand-edits inside .pio/libdeps/<env>/esp-nimble-cpp/, which is gitignored
build output, not source-controlled. They were invisible to a fresh clone —
including this repo's own db-radar env rename, whose new libdeps directory
exposed the gap immediately. Automating them here is what makes `pio run`
work out of the box for anyone who clones the repo fresh, same motivation
as scripts/patch_i2c_master_nack_hang.py.

PATCH 1 — src/NimBLEAttValue.h: `time_t` is used in both the
CONFIG_NIMBLE_CPP_ATT_VALUE_TIMESTAMP_ENABLED branch and the disabled one,
but <time.h> is only #included inside the enabled branch — a library bug,
not an IDF-version issue. Include it unconditionally.

PATCH 2 — src/NimBLEDevice.cpp: NimBLEDevice::deinit() calls
esp_nimble_hci_and_controller_deinit(), which was removed from ESP-IDF 5.x.
Replaced with the two calls it used to wrap: esp_bt_controller_disable()
then esp_bt_controller_deinit() (esp_bt.h is already included in this file
for other calls, so no new include is needed).

Idempotent and safe by construction, same as the I2C patch script: each
patch applies only if its exact known-bad block is found untouched; if the
pinned esp-nimble-cpp tag ever changes and the surrounding code no longer
matches byte-for-byte, this prints a loud warning and leaves the file alone
rather than guessing at a fuzzy patch.
"""
Import("env")
import os

LIB_REL = "esp-nimble-cpp"

ATTVALUE_REL = os.path.join("src", "NimBLEAttValue.h")
ATTVALUE_OLD = """#if CONFIG_NIMBLE_CPP_ATT_VALUE_TIMESTAMP_ENABLED
#    include <time.h>
#endif
"""
ATTVALUE_NEW = """// PATCHED (scripts/patch_nimble_cpp_idf5.py, 2026-08-09): time_t is used in both
// branches below, but upstream only #included <time.h> in the enabled one — a
// library bug, not an IDF-version issue. Include it unconditionally.
#include <time.h>
"""

DEVICE_REL = os.path.join("src", "NimBLEDevice.cpp")
DEVICE_OLD = """#ifdef ESP_PLATFORM
        ret = esp_nimble_hci_and_controller_deinit();
        if (ret != ESP_OK) {
            NIMBLE_LOGE(LOG_TAG, "esp_nimble_hci_and_controller_deinit() failed with error: %d", ret);
        }
#endif
"""
DEVICE_NEW = """#ifdef ESP_PLATFORM
        // PATCHED (scripts/patch_nimble_cpp_idf5.py, 2026-08-09):
        // esp_nimble_hci_and_controller_deinit() was removed in ESP-IDF 5.x. Replaced
        // with the two calls it used to wrap internally.
        esp_bt_controller_disable();
        ret = esp_bt_controller_deinit();
        if (ret != ESP_OK) {
            NIMBLE_LOGE(LOG_TAG, "esp_bt_controller_deinit() failed with error: %d", ret);
        }
#endif
"""


def apply_patch(lib_dir, rel_path, old_block, new_block, tag):
    target_path = os.path.join(lib_dir, rel_path)
    if not os.path.isfile(target_path):
        print("[NIMBLE_PATCH] WARNING: {} not found — skipping {}".format(target_path, tag))
        return

    with open(target_path, "r") as f:
        content = f.read()

    if new_block in content:
        print("[NIMBLE_PATCH] {} already applied — nothing to do".format(tag))
        return

    if old_block not in content:
        print("=" * 78)
        print("[NIMBLE_PATCH] WARNING: expected unpatched block not found for {}.".format(tag))
        print("[NIMBLE_PATCH] The pinned esp-nimble-cpp version likely changed since this")
        print("[NIMBLE_PATCH] script was written. THIS PATCH WAS NOT APPLIED.")
        print("[NIMBLE_PATCH] See scripts/patch_nimble_cpp_idf5.py and re-check whether the")
        print("[NIMBLE_PATCH] pinned tag still needs it.")
        print("=" * 78)
        return

    content = content.replace(old_block, new_block, 1)
    with open(target_path, "w") as f:
        f.write(content)

    print("[NIMBLE_PATCH] Applied {} to {}".format(tag, target_path))


def main():
    lib_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"], LIB_REL)
    if not os.path.isdir(lib_dir):
        print("[NIMBLE_PATCH] WARNING: {} not found — skipping (library not installed yet?)".format(lib_dir))
        return

    apply_patch(lib_dir, ATTVALUE_REL, ATTVALUE_OLD, ATTVALUE_NEW,
                "NimBLEAttValue.h time_t/<time.h> fix")
    apply_patch(lib_dir, DEVICE_REL, DEVICE_OLD, DEVICE_NEW,
                "NimBLEDevice.cpp deinit() IDF5 fix")


main()
