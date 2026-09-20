# Changelog

All notable changes to the Shelly Gen4 Matter Module firmware. Downloads for
every version are on the [Releases page](https://github.com/hencou/shelly_gen4_matter_module/releases).
The Matter `SoftwareVersion` is bumped with every release so controllers offer
the update over Matter OTA.

## 1.6.6 — 2026-09-07

- Factory reset by holding the **onboard PCB button for 30 seconds**. Only the
  PCB button triggers it; the wall-switch (SW/SW2) and Add-on inputs never do.
  Uses the same safe path as the dashboard button: flag, reboot, wipe NVS
  before Matter/OpenThread start. The status LED blinks fast once accepted.

## 1.6.5 — 2026-09-06

- WiFi has three modes on the dashboard: **Off**, **10 minutes** or **Always on**,
  next to Thread, switched at runtime without a reboot.
- Removing the module from a Matter controller re-opens the commissioning
  window (other fabrics stay); removing the last controller reboots into BLE
  commissioning. The module can always be brought back online without physical
  access.
- Group KeySet, group and binding limits raised so every endpoint can hold at
  least 4 group bindings (12 per module).
- Web factory reset wipes NVS on the next boot, before Matter/OpenThread start,
  instead of underneath them (fixes an OpenThread assert during reset).
- `/api/diag` lists the commissioned fabrics (index, compressed fabric ID, node,
  vendor, label).

## 1.6.4 — 2026-08-31

- **Thread-first boot**: an uncommissioned module starts straight in BLE
  commissioning, a commissioned one straight in Thread. The WiFi setup mode and
  the reboot-based WiFi/Thread mode switches are gone.
- **Temporary WiFi next to Thread**: 6× button press or the dashboard opens a
  10-minute WiFi window (STA with stored credentials, SoftAP `192.168.4.1`
  fallback) while Thread and Matter stay reachable; WiFi is torn down again
  without a reboot.
- SRP fallback server: when no Thread border router advertises a service
  registry, one module elects itself so Matter service discovery keeps working.
- Thread watchdog recovers a node that boots detached or as an MTD.
- Many stability fixes for radio sharing, heap sizing, HTTP socket handling and
  larger Lua script uploads.
- Documented the active-low/high polarity of every GPIO.

## 1.6.3 — 2026-08-23

- Unicast binding: a stale peer address no longer delays the first button press
  by 5 s, and a retried command is never delivered twice.

## 1.6.2 — 2026-08-22

- Built against **esp-matter release/v1.6 (Matter 1.6.0)** and ESP-IDF v5.5.5.
- Unicast bindings recover from a dead CASE session instead of dropping the
  command.
- Software version reported correctly so controllers stop offering an update for
  the running firmware.

## 1.6.1 — 2026-08-17

- Per-model GPIO map verified against the official Shelly stock firmware images
  (see `STOCK_GPIO.md`).
- Device log viewable in the dashboard (Log tab); DS18B20 1-Wire timing fixes.
- Management HTTP server starts on commissioning-complete instead of the next
  boot.

## 1.6.0 — 2026-08-10

- **Install from the stock Shelly web UI**: upload the `-ota.zip` package, no
  UART and no opening the device.
- **Return to stock** from the management page: fetches the official firmware
  for the module's model and restores the stock loader, partition table and
  boot state (hardware-verified).
- Own ESP-IDF bootloader with A/B OTA and rollback, migrated automatically on
  first boot.
- Illuminance Sensor endpoint (lux from Lua).

## 1.5.9 — 2026-08-05

- Temperature Sensor endpoint fix (MeasuredValue stayed null).
- Remote WiFi/Thread mode switch buttons; `tools/shelly-overview.sh` lists all
  modules found via mDNS/SRP.

## 1.5.4 — 2026-07-20 (through 1.5.8)

- **Shelly 2PM Gen4** support (dual relay, ADE7953 two-channel metering).
- Contact Sensor (BooleanState) endpoint settable from Lua.
- Management dashboard served over Thread (IPv6) as well as WiFi.
- Internal chip temperature available in Lua.
- Project renamed to `shelly_gen4_matter_module`.

## 1.5.3 — 2026-07-06

- **Multi-model firmware**: one image for Shelly 1 Gen4, 1 Mini Gen4 and 1PM
  Gen4, selected on the dashboard; BL0942 power metering on the 1PM.
- SRP server only as fallback, yielding to any Thread border router.
- Thread connectivity watchdog.
- Documented all OTA methods and installation from stock firmware.

## 1.5.1 — 2026-06-24

- Stock-compatible partition layout (partition table at 0x10000) so the Shelly
  web UI OTA path works.
- Matter data stored in the NVS partition; optional NVS dump in backups.
- Commission Mode button, chip_kvs backup/restore, `/api/diag` endpoint.

## 1.5 — 2026-06-16

- Matter OTA `.ota` build script and Shelly web UI `.zip` package.
- WiFi 10-minute auto-off.

## 1.4 — 2026-06-15

- Thread SRP server mode: Matter DNS-SD without an external border router.
- Group multicast bindings from firmware (KeySet, GroupKeyMap, ACL) so lamps
  accept multicast commands.

## 1.3 — 2026-06-09

- **Lua 5.4 scripting platform**: configurable Matter endpoints per script slot,
  edited from the web interface; dynamic endpoints based on slot configuration.
- Management dashboard with WiFi, Hardware, Scripts and Backup tabs.
- Smart boot: WiFi setup mode vs BLE commissioning mode.

## 1.2 — 2026-06-06

- Group setup script supports multiple switches; AddGroup/GroupKeyMap ordering
  fixes.

## 1.0 — 2026-06-05

- First release: Shelly 1 Gen4 as Matter-over-Thread light switch with
  dimming and colour-temperature gestures, relay endpoint, Shelly Plus Add-on
  inputs (DS18B20, digital, analog), Matter OTA requestor.
