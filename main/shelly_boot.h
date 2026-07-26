#pragma once

#include "esp_err.h"

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

#ifdef __cplusplus
}
#endif
