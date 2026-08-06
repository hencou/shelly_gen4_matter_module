#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-time migration from the stock Shelly OS loader to our ESP-IDF bootloader.
 *
 * Install-from-stock ships a bootloader-less package (see make-webui-ota-zip.py):
 * the stock updater keeps its own "Shelly OS loader" and boots our app in the
 * inactive slot via its SH0S boot-select. That path is reliable, but leaves the
 * device dependent on the reverse-engineered SH0S format and on future changes
 * to the Shelly loader.
 *
 * loader_migrate_maybe() closes that gap automatically: on the first boot that
 * still runs under the stock loader it writes a valid ESP-IDF otadata entry for
 * the currently running slot, flashes our embedded ESP-IDF bootloader to flash
 * offset 0x0, and reboots. From then on the device boots via the standard
 * ESP-IDF bootloader + otadata and OTA no longer touches SH0S.
 *
 * It is idempotent and self-terminating: once our bootloader is in place the
 * loader detection no longer sees the stock loader, so it does nothing. A small
 * NVS attempt counter prevents a boot loop if a write ever fails (the app keeps
 * running fine under the stock loader in that case).
 *
 * Call once early in app_main, after nvs_flash_init(). Requires that the units
 * are not flash-encrypted (confirmed for Shelly Gen4).
 */
void loader_migrate_maybe(void);

#ifdef __cplusplus
}
#endif
