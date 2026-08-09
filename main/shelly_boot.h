#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stock Shelly OS loader boot-select support.
 *
 * Shelly Gen4 devices ship with a proprietary second-stage bootloader
 * ("Shelly OS loader") that does NOT use the standard ESP-IDF otadata
 * format. It keeps its own A/B boot-select structure ("SH0S") in the
 * otadata partition and boots the app slot recorded in that structure,
 * ignoring esp_ota_set_boot_partition(). This module writes a valid
 * SH0S entry so the stock loader switches to the newly flashed slot,
 * which keeps the ability to flash to/from stock Shelly firmware.
 */

/*
 * Switch the stock Shelly loader's active app slot.
 *   slot = 0 -> app_0, slot = 1 -> app_1.
 *
 * Returns:
 *   ESP_OK               boot-select updated for the stock loader.
 *   ESP_ERR_INVALID_STATE no valid SH0S entry found (device is not running
 *                         the stock Shelly loader) -- caller should fall
 *                         back to esp_ota_set_boot_partition().
 *   other esp_err_t       flash/partition error.
 */
esp_err_t shelly_boot_switch_slot(int slot);

/*
 * Patch an in-RAM copy of a stock `boot_state.bin` so its SH0S entries select
 * `slot` (0 -> app_0, 1 -> app_1), mark it committed and clear the boot-attempt
 * counter, recomputing both CRCs. `state`/`len` cover the whole otadata image;
 * every 0x1000 sector carrying the SH0S magic is patched.
 *
 * Return-to-stock uses this instead of shelly_boot_switch_slot(): the stock
 * package ships a valid boot state, but it hard-codes app_1 while the stock app
 * is written to whichever slot is currently inactive. Patching the buffer before
 * it is flashed means the boot state is correct in a single write, with no
 * dependency on reading a valid SH0S back from flash -- which is impossible
 * while the ESP-IDF bootloader is still installed (the stock loader is written
 * last, on purpose).
 *
 * Returns ESP_ERR_NOT_FOUND when the buffer holds no SH0S entry at all.
 */
esp_err_t shelly_boot_patch_state(uint8_t *state, size_t len, int slot);

/*
 * Cache the current valid SH0S entry into RAM. Call this once early at boot,
 * before any OTA runs. It lets shelly_boot_switch_slot() still rebuild a valid
 * SH0S record even after an IDF OTA path (e.g. the Matter OTA requestor, which
 * calls esp_ota_set_boot_partition()) has overwritten the live otadata sectors
 * with the ESP-IDF select format. Without this snapshot such a device would be
 * left with no valid SH0S and the stock loader would not boot the new slot.
 *
 * Does nothing (and leaves no snapshot) when the device is not running the
 * stock Shelly loader, so IDF-bootloader builds keep their normal behaviour.
 */
void shelly_boot_snapshot(void);

/*
 * Detect which second-stage bootloader is installed at flash offset 0x0.
 *
 * Returns true when the stock "Shelly OS loader" is present (its identifying
 * string is embedded in the bootloader image), false when a plain ESP-IDF
 * bootloader is present. The result is read from flash once and cached.
 *
 * Callers use this to pick the correct boot-select mechanism: SH0S
 * (shelly_boot_switch_slot) on the stock loader, esp_ota_set_boot_partition()
 * on the ESP-IDF bootloader. New installs ship the ESP-IDF bootloader; devices
 * flashed with older packages may still carry the stock loader, so both paths
 * must keep working.
 */
bool shelly_loader_present(void);

#ifdef __cplusplus
}
#endif
