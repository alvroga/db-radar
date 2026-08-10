"""
patch_nimble_cpp_idf5.py — PlatformIO extra_script (pre-build)

Compile- and runtime-breaking bugs in esp-nimble-cpp v1.4.1 (this project's
pinned version, platformio.ini's lib_deps) when built against ESP-IDF 5.x
with CONFIG_BT_NIMBLE_ENABLED (native IDF NimBLE host). v1.4.x was written
against IDF 4.x/5.0; these were never fixed upstream on the v1.4.1 tag.

FOUND THE HARD WAY (2026-08-09): patches 1 and 2 previously existed only as
hand-edits inside .pio/libdeps/<env>/esp-nimble-cpp/, which is gitignored
build output, not source-controlled. They were invisible to a fresh clone —
including this repo's own db-radar env rename, whose new libdeps directory
exposed the gap immediately (compile failure, loud). Patch 3 was a second,
quieter casualty of the same rename: a runtime-only bug, so the rename's
"verified with a from-scratch build" check (compile success only) didn't
catch it — it surfaced hours later as a boot-time NimBLE crash, not a build
failure, and had to be bisected back to this rename before being found here.
Automating all three is what makes `pio run` work out of the box for anyone
who clones the repo fresh AND boots correctly, same motivation as
scripts/patch_i2c_master_nack_hang.py.

PATCH 1 — src/NimBLEAttValue.h: `time_t` is used in both the
CONFIG_NIMBLE_CPP_ATT_VALUE_TIMESTAMP_ENABLED branch and the disabled one,
but <time.h> is only #included inside the enabled branch — a library bug,
not an IDF-version issue. Include it unconditionally.

PATCH 2 — src/NimBLEDevice.cpp: NimBLEDevice::deinit() calls
esp_nimble_hci_and_controller_deinit(), which was removed from ESP-IDF 5.x.
Replaced with the two calls it used to wrap: esp_bt_controller_disable()
then esp_bt_controller_deinit() (esp_bt.h is already included in this file
for other calls, so no new include is needed).

PATCH 3 — src/NimBLEDevice.cpp: NimBLEDevice::init() unconditionally does a
manual esp_bt_controller_init()/enable()/esp_nimble_hci_init() sequence,
THEN calls the IDF-native nimble_port_init() right after with no `#if`
between them. Under CONFIG_BT_NIMBLE_ENABLED (this project's config),
nimble_port_init() already performs esp_bt_controller_init()/enable()
itself (see esp-idf's nimble_port.c) — so the controller gets initialized
twice. The second (nimble_port_init()'s own) call fails with
ESP_ERR_INVALID_STATE, logs "BLE_INIT: controller init failed", and
nimble_port_init() returns early — skipping the esp_nimble_init() call
that sets up the host stack's internal pools/queues. NimBLEDevice::init()
never checks nimble_port_init()'s return value, so it plows ahead into
ble_svc_gap_device_name_set()/nimble_port_freertos_init() against a host
stack that was never actually initialized: Guru Meditation LoadProhibited,
moments after the "controller init failed" log line. Fixed by skipping the
manual block entirely under CONFIG_BT_NIMBLE_ENABLED — nimble_port_init()
already does the job; this project never calls setScanDuplicateCacheSize()/
setScanFilterMode() (the only things the manual bt_cfg block customizes
beyond the default), so nothing is lost by not building bt_cfg by hand.

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

DEVICE_DBLINIT_OLD = """        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if  defined (CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
        bt_cfg.bluetooth_mode = ESP_BT_MODE_BLE;
#else
        bt_cfg.mode = ESP_BT_MODE_BLE;
        bt_cfg.ble_max_conn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#endif
        bt_cfg.normal_adv_size = m_scanDuplicateSize;
        bt_cfg.scan_duplicate_type = m_scanFilterMode;

        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
        ESP_ERROR_CHECK(esp_nimble_hci_init());
"""
DEVICE_DBLINIT_NEW = """        // PATCHED (scripts/patch_nimble_cpp_idf5.py, 2026-08-09): this manual
        // controller bring-up ran unconditionally, immediately followed (no #if
        // between them) by the IDF-native nimble_port_init() below — which, under
        // CONFIG_BT_NIMBLE_ENABLED, already calls esp_bt_controller_init()/enable()
        // itself. The second (nimble_port_init()'s) call failed with
        // ESP_ERR_INVALID_STATE ("BLE_INIT: controller init failed"), which made
        // nimble_port_init() return early — skipping esp_nimble_init() — and
        // NimBLEDevice::init() never checked that return value, so it went on to
        // use an uninitialized host stack: Guru Meditation LoadProhibited moments
        // later. Skipped entirely under CONFIG_BT_NIMBLE_ENABLED; nimble_port_init()
        // does this project's job. (bt_cfg's normal_adv_size/scan_duplicate_type
        // customization is lost with it, but this project never calls
        // setScanDuplicateCacheSize()/setScanFilterMode(), so nothing changes.)
#ifndef CONFIG_BT_NIMBLE_ENABLED
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if  defined (CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
        bt_cfg.bluetooth_mode = ESP_BT_MODE_BLE;
#else
        bt_cfg.mode = ESP_BT_MODE_BLE;
        bt_cfg.ble_max_conn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#endif
        bt_cfg.normal_adv_size = m_scanDuplicateSize;
        bt_cfg.scan_duplicate_type = m_scanFilterMode;

        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
        ESP_ERROR_CHECK(esp_nimble_hci_init());
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
    apply_patch(lib_dir, DEVICE_REL, DEVICE_DBLINIT_OLD, DEVICE_DBLINIT_NEW,
                "NimBLEDevice.cpp init() double controller-init fix")


main()
