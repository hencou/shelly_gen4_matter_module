# shelly_gen4_matter_module

**Custom Matter-over-Thread firmware** for the **Shelly Gen4** line (ESP32-C6) — one image for the Shelly 1, 1PM and 2PM Gen4 — with **Lua scripting** for fully configurable endpoints.

## Features

- **Dynamic Matter endpoints** — no hard-coded endpoints. Configure via the web management dashboard
- **Lua 5.4 scripting** — write custom button/relay/sensor logic per endpoint slot (up to 8 slots)
- **Matter 1.5** compatible — works with Home Assistant, Google Home, Apple Home
- **Thread + WiFi** — Thread for Matter communication, WiFi for management/OTA
- **Smart boot** — auto-detects factory reset vs configured vs commissioned state
- **WiFi management dashboard** — configure scripts, WiFi, endpoints, backup/restore
- **Over-the-air updates** — Matter OTA over Thread and `.bin` upload via the dashboard (standard ESP-IDF OTA with rollback). Can be installed straight from the stock Shelly web UI (no UART, no opening the device); UART is only needed to make a full backup or for a guaranteed return to stock. See [Firmware updates](#firmware-updates).

## Endpoint types

Each script slot can be configured as one of these Matter endpoint types:

| Type | Matter device | Description |
|---|---|---|
| **OnOff Toggle + Dim + Color** | 0x0103 Light Switch (client) | Toggle, dim, color temp via bindings |
| **OnOff State-follow** | 0x0103 Light Switch (client) | On/Off follows switch position |
| **Temperature Sensor** | 0x0302 Temp. Sensor (server) | DS18B20 via 1-Wire |
| **Occupancy Sensor** | 0x0107 Occupancy Sensor (server) | Analog IN duty cycle |
| **Relay (OnOff Light)** | 0x0100 OnOff Light (server) | Physical relay (GPIO from the active hardware profile; 2 relays on the 2PM) |

## Based on

- [esp-matter](https://github.com/espressif/esp-matter) with **Matter 1.5** support
- [connectedhomeip](https://github.com/project-chip/connectedhomeip) (as submodule within esp-matter)
- ESP-IDF v5.5.4
- Lua 5.4 (compiled as component)

## Target hardware

One firmware image supports four Gen4 models (all ESP32-C6, 8 MB flash). Select
the model on the management dashboard (**Hardware → Device Type**); the choice is
stored in NVS and applied on the next boot. The correct GPIO mapping is then used
for the relay(s), wall-switch input(s), onboard button and status LED.

| Model | Relay | Switch | Button | Status LED | Add-on | Power meter |
|---|---|---|---|---|---|---|
| **Shelly 1 Gen4** (default) | GPIO5 | GPIO10 | GPIO4 | GPIO15 | yes | — |
| **Shelly 1 Mini Gen4** | GPIO10 | GPIO12 | GPIO22 | GPIO5 | — | — |
| **Shelly 1PM Gen4** | GPIO4 | GPIO10 | GPIO1 | GPIO0 | yes | BL0942 (UART1 TX=GPIO6 RX=GPIO7, 9600 baud) |
| **Shelly 2PM Gen4** | GPIO5 + GPIO3 | GPIO11 + GPIO10 | GPIO12 | GPIO0 | yes | ADE7953 dual-channel (I2C SDA=GPIO6 SCL=GPIO7, IRQ=GPIO19) |

> ℹ️ **Shelly 1 Mini Gen4 is supported again.** It was previously excluded
> because the Mini has no accessible UART pads and the only install path was a
> one-time UART flash. Since installing straight from the stock Shelly web UI now
> works (see [Firmware updates](#firmware-updates)), the Mini can be flashed
> without opening it. The Mini has **no** Shelly Plus Add-on connector, so the
> add-on inputs are unavailable on it.

> ⚠️ **Test status:** only the **Shelly 1 Gen4** has been verified on real
> hardware. The **1 Mini Gen4**, **1PM Gen4** and **2PM Gen4** profiles are
> implemented from the published pinouts but have **not** been hardware-tested.
> The BL0942 and ADE7953 scaling constants are placeholders and **must** be
> calibrated against a known load on real hardware before the reported
> voltage/current/power values are trustworthy.

> ⚠️ **2PM pinout conflict — VERIFY before connecting mains.** The ESPHome device
> DB (https://devices.esphome.io/devices/shelly-plus-2pm-gen-4/) is internally
> inconsistent: its human-readable "GPIO Pinout" table swaps relay ↔ switch on
> GPIO5/GPIO3/GPIO11/GPIO10 relative to its working YAML config. This firmware
> follows the **YAML config** (relays on GPIO5/GPIO3, switches on GPIO11/GPIO10).
> Confirm the mapping on your own 2PM before wiring it to mains — driving a
> switch-input pin as a relay output can damage the device.

Notes:
- **Changing the device type does not require Matter re-commissioning** — the
  firmware exposes a generic switch model, so a type change is only a GPIO remap.
- **Warning:** selecting the wrong model drives the wrong GPIOs. Pick the model
  that matches your physical hardware.
- The **Shelly Plus Add-on** (DS18B20 + touch + analog occupancy) is available on
  the 1 Gen4, 1PM Gen4 and 2PM Gen4.
- On the **1PM Gen4** the BL0942 reports voltage, current, active power,
  accumulated energy and line frequency via a Matter **Electrical Power
  Measurement** endpoint, and on the dashboard Hardware tab.
- The **2PM Gen4** uses an ADE7953 measuring two channels (A = relay 1,
  B = relay 2). Each channel is exposed as its own **Electrical Power
  Measurement** endpoint. The two relays are two OnOff Light endpoints and both
  wall-switch inputs are reported to scripts (see the Lua section).

| Component | Details |
|---|---|
| Shelly Plus Add-on | DS18B20 (TX=GPIO9/RX=GPIO16) + TTP223 touch (GPIO18) + Analog IN (GPIO17) |
| Thread Border Router | Google TV Streamer 4K (or any Thread BR) |
| Matter controller | Home Assistant Matter Server, Google Home, Apple Home |
| Commissioning | HA Matter Server UI or `chip-tool` |

## Setup procedure

### 1. First flash

There are two ways to install the firmware for the first time.

**Option A — from the stock Shelly web UI (no UART, no opening the device):**
build the web-UI package and upload it through the stock Shelly device page
(**Settings → Firmware**, "install from file"):

```bash
idf.py build
python3 tools/make-webui-ota-zip.py     # → shelly-gen4-matter-module-v<version>-ota.zip
```

The package ships **no bootloader** — only `nvs`, `app` and `fs`. It keeps the
stock "Shelly OS loader" so the stock updater's own A/B flow reliably boots our
app in the inactive slot. On the first boot the firmware then performs a
**one-time self-migration**: it writes our ESP-IDF bootloader to `0x0` plus
valid `otadata` and reboots. From then on the module boots via the standard
ESP-IDF bootloader + `otadata`, so OTA no longer depends on the stock loader's
proprietary `SH0S` boot-select or on future Shelly loader changes. This is the
only way to install on a **Shelly 1 Mini Gen4**, which has no accessible UART pads.

> ⚠️ **Update the device to stock Shelly v2.0 first, then flash our package.**
> Fresh units often ship as a *dual-variant* build with a Matter app (`S1G4`) in
> one slot and a Zigbee app (`S1G4ZB`) in the other. Stock 2.0 has a firmware
> **variant guard** (`shelly_alternative.c: Not switching fw variant`) that
> refuses to boot a foreign app on such a unit, so the install appears to "revert
> to stock". Running the normal stock **1.5.x → 2.0** update first collapses the
> unit to a single Matter variant (both slots `S1G4`), after which our web-UI
> package installs correctly. Do the stock 2.0 update, reboot, *then* upload our
> `.zip`.

> ✅ Verified on a **Shelly 1 Gen4** on stock **v2.0** (both after a factory-2.0
> unit was updated 1.5.99 → 2.0, and on a unit already running 2.0). The layout is
> identical on stock 1.5/1.7/2.0 (same offsets). These units are not
> flash-encrypted, so writing our plaintext ESP-IDF bootloader is safe.
> Keep a full backup before flashing (see the warning under
> [Firmware updates](#firmware-updates)).

**Option B — UART flash (see [INSTALL.md](INSTALL.md)):** open the device and
wire a USB-UART adapter to the J6 connector. This installs the ESP-IDF bootloader
together with the partition table and app, and is also the way to make a full
8 MB backup of the stock Shelly firmware for a guaranteed return to stock.

After either first install, **all** further updates are over the air (Matter OTA
/ dashboard `.bin` upload, or a fresh web-UI package) — no UART needed.

### 2. Factory reset → WiFi setup mode

After flashing (or factory reset via the web interface), the module boots into **WiFi setup mode**:

1. WiFi enabled, Bluetooth disabled
2. STA connection attempt with compile-time credentials from `main/secrets.h`
3. **STA succeeds** → credentials saved to NVS, management dashboard on router IP
4. **STA fails** → fallback to AP mode: `shelly-cfg-XXXX` (open network, `http://192.168.4.1/`)

### 3. Configure endpoints via dashboard

Open the management dashboard in your browser and go to the **Scripts** tab:

1. Set a **name**, **endpoint type**, **trigger**, and **Lua script** for each slot you need
2. Click **Save** for each slot
3. Click **Reboot** on the Scripts page

See [SCRIPTS.md](SCRIPTS.md) for example scripts.

### 4. Commissioning

After reboot with configured endpoints, the module enters **BLE commissioning mode**:

1. Open Home Assistant → Settings → Devices & Services → Matter → "Add device"
2. Enter setup code: **34970112332** (default, configurable in `sdkconfig.defaults`)
3. HA Matter Server pairs via BLE, provisions Thread credentials
4. After ~30-60s the device appears in HA with the configured endpoints

### 5. Normal operation

After commissioning, Thread + Bluetooth are active for Matter communication. WiFi is off by default.

#### Reach the management dashboard over Thread

The dashboard is served over IPv6 on the Thread network, so you can reach it **without WiFi**:

- **Direct (always works):** the module logs its addresses at boot. Use the **OMR / SLAAC** address (marked `<-- OMR` in the log, e.g. `fd96:…`) and open `http://[<omr-ipv6>]/` from any host that routes to the Thread network through your border router. The mesh-local (`fd…:0:0:ff:fe00:…`) and link-local (`fe80:…`) addresses are not routable off-mesh.
- **By name (mDNS):** the module advertises an `_http._tcp` service (instance label = configured hostname) via the Matter SRP client. A border router with an advertising proxy re-publishes it as LAN mDNS, so you can discover it with `avahi-browse -rt _http._tcp` / `dns-sd -B _http._tcp`, or generate a clickable overview of all modules with [`tools/shelly-overview.sh`](tools/shelly-overview.sh). Note the resolvable `.local` name is the opaque CHIP SRP host (e.g. `52E2….local`), not the friendly hostname — the friendly hostname is the service label you browse.

#### SRP fallback server (discovery without a border router)

Direct device-to-device control (a switch bound to a lamp) needs the switch to
**resolve** the lamp's operational address over Thread DNS-SD. That resolution
is answered by the Thread network's SRP / DNS-SD server. Some border routers
register services and proxy them to LAN mDNS, but do **not** answer the
operational-discovery query a Thread node makes for another node — so a freshly
reset/commissioned switch times out while resolving the lamp, even though the
lamp is reachable and works from Home Assistant.

> **Not available on this SDK.** Running an on-device SRP / DNS-SD *server*
> requires `CONFIG_OPENTHREAD_BORDER_ROUTER=y`, which on ESP-IDF v5.5.4 wires
> the ESP border-router glue. After commissioning, the Thread stack calls
> `otThreadSetEnabled(true)` (only when a dataset is stored), which hangs on a
> Thread-only device that has no infra/backbone interface — the module never
> boots past Thread attach on any reboot after commissioning. The prebuilt
> `libopenthread_br.a` cannot be slimmed either (its MeshCoP mDNS publisher
> references `otBackboneRouterGetState` / `otBorderRoutingGetOmrPrefix`
> unconditionally). Therefore `CONFIG_OPENTHREAD_BORDER_ROUTER` is kept **off**
> and the `srp_mode` fallback-server code is compiled out. The election logic
> (`matter_srp_server_*`) is retained behind `#if CONFIG_OPENTHREAD_BORDER_ROUTER`
> for a future SDK that can run the SRP server without a backbone interface.
>
> To make freshly reset/commissioned switches resolve lamps for direct bindings,
> add a real Thread border router with an advertising proxy that answers
> operational-discovery queries (e.g. a Home Assistant OpenThread Border Router
> on a ZBT-1/SkyConnect dongle or HA Yellow). Alternatively, drive the lamp from
> a controller (Home Assistant automation), which needs no on-device resolve.

#### Switch to WiFi for faster management

Two buttons on the dashboard (**WiFi & OTA** tab) reboot the device into a specific mode — handy for hard-to-reach devices you can only reach over Thread:

- **Restart to WiFi mode** — reboots into WiFi (STA if credentials are saved, otherwise the `shelly-cfg-XXXX` SoftAP on `192.168.4.1`) and serves the dashboard there. Matter/Thread stays down while in this mode. After **10 minutes** it automatically reboots back to Thread mode, so you never lose the device permanently.
- **Restart to Thread mode** — reboots straight back into normal Matter-over-Thread operation.

The physical shortcut still works too: **press any button 6× rapidly** (within 2.5 seconds) to disable Thread and start WiFi in APSTA mode at runtime (AP `shelly-cfg-XXXX` on `192.168.4.1`, plus STA if credentials are saved).

### Backup & restore

Via the management dashboard → **Backup** tab:
- **Download Backup** — exports all settings as a JSON file: WiFi credentials + all 8 script slot configurations (name, type, trigger, period, Lua code)
- **Restore Backup** — upload a previously downloaded JSON backup to restore all settings. The device reboots automatically after restore.

### Factory reset

Via the web management dashboard → **Factory Reset** button. This wipes:
- All NVS data (WiFi credentials, script configurations, bench mode)
- All Matter fabrics and commissioning data (NVS namespaces)

After factory reset the module reboots into WiFi setup mode (step 2).

## Firmware updates

Once the custom firmware is running you can update it three ways. They all flash the **same** application binary (`build/shelly_gen4_matter_module.bin`) — they only differ in transport, and all use standard ESP-IDF OTA slot selection with bootloader rollback (a bad image is automatically reverted to the previous slot).

> ⚠️ **Keep a full UART backup as your guaranteed way back.** The management
> page can flash an original Shelly firmware package back onto the device (see
> [Return to stock](#4-return-to-stock-shelly-firmware) below), but that path is
> **not hardware-tested**. The one route that always
> works is restoring the full 8 MB UART backup of that exact device, so make
> that backup **before** the first flash (with
> [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect/)
> or `esptool.py read_flash`, see [INSTALL.md](INSTALL.md)) — afterwards it is
> too late. The factory `shelly` partition (hardware/Matter credentials) is
> never overwritten and is preserved across installs.

### 1. Matter OTA (over Thread)

Update over the existing Thread/Matter connection — no WiFi or cabling needed. Build the `.ota` image and serve it from a Matter OTA provider (e.g. Home Assistant):

```bash
idf.py build
python3 tools/make-matter-ota.py      # → shelly-gen4-matter-module-v<version>.ota
```

The image embeds the vendor/product ID and software version; the device only accepts an image with a higher software version than it currently runs. Bump `CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION[_STRING]` in `main/CHIPProjectConfig.h` for each release.

> Tip: `make-matter-ota.py` just wraps whatever is in `build/`. Always `idf.py build` first (use `idf.py fullclean` to force a genuine recompile) and confirm the `.ota` is fresh — a stale `build/` produces a stale `.ota`.

### 2. `.bin` upload via the management dashboard

For modules already running this firmware: open the management dashboard (**6× rapid button press** → WiFi), go to the **WiFi & OTA** tab, and either provide a firmware URL or upload `build/shelly_gen4_matter_module.bin` directly. The device flashes the inactive OTA slot with `esp_ota_set_boot_partition()` and reboots into it; on boot the app marks itself valid, otherwise the bootloader rolls back.

### 3. Web-UI package (also for installing from stock Shelly)

Build a Shelly web-UI package and upload it through a Shelly device page (the stock page when installing from stock, or the module's own web UI later):

```bash
idf.py build
python3 tools/make-webui-ota-zip.py     # → shelly-gen4-matter-module-v<version>-ota.zip
```

The package ships **no bootloader** and **no partition table** — only `nvs`, `app` and `fs`. It keeps the stock "Shelly OS loader" so the stock v2.0 updater's own A/B flow installs our app into the inactive slot and boots it (shipping a foreign bootloader instead breaks this: the stock updater is `SH0S`-driven and an ESP-IDF bootloader cannot follow that boot-select). On the first boot under the stock loader the firmware performs a **one-time self-migration** (`main/loader_migrate.c`): it writes a valid ESP-IDF `otadata` for the running slot, flashes our embedded ESP-IDF bootloader to `0x0`, and reboots. From then on the module boots via the standard ESP-IDF bootloader + `otadata`, so OTA uses plain `esp_ota_set_boot_partition()` and no longer depends on the reverse-engineered stock `SH0S` boot-select or on future Shelly loader changes. These units are not flash-encrypted, so writing our plaintext bootloader is safe. This is the route used for the very first install from stock Shelly firmware (see [First flash](#1-first-flash)) and works regardless of whether the unit runs stock 1.5/1.7/2.0.

> The migration is idempotent and self-terminating: once our ESP-IDF bootloader is at `0x0` the loader detection no longer sees the stock loader, so it never runs again. If a migration write ever fails, the app keeps running fine under the stock loader (OTA then uses `SH0S`); the firmware auto-detects which bootloader is present and picks the matching boot-select, so both paths keep working.

> **Install from stock: update to stock v2.0 first.** On a factory unit that still
> carries the dual-variant layout (`S1G4` Matter + `S1G4ZB` Zigbee), stock 2.0's
> variant guard refuses to switch to a foreign app and the module keeps booting
> stock. Run the stock **1.5.x → 2.0** update first (collapses it to a single
> `S1G4` variant), reboot, then upload this package. See the warning under
> [First flash](#1-first-flash).

### 4. Return to stock Shelly firmware

The management dashboard (**Backup** tab) can flash an **original Shelly firmware package** back onto the module — the same `.zip` the stock web UI consumes. Download the package matching the model on your device label from the [community firmware archive](https://archive.shelly-tools.de/) (e.g. `S4SW-001X16EU` = Shelly 1 Gen4), then upload it under *Return to stock Shelly firmware*.

The stock app cannot run under our ESP-IDF bootloader (it needs the Shelly OS loader and an `SH0S` boot state), so the module is made **byte-for-byte stock** again. The firmware first writes the stock **app** to the inactive slot and the stock **filesystem**, and verifies their SHA-256 — nothing outside that inactive slot is touched until this succeeds. It then restores, in order, the stock **boot state** (`otadata`), the stock **partition table** (`0x10000`), points the `SH0S` boot-select at the slot the stock app landed in, and finally rewrites the stock **bootloader** (Shelly OS loader) at `0x0`. Every write is verified by read-back. The factory `shelly` partition is never touched. The units are not flash-encrypted, so the plaintext images from the package reproduce the stock layout exactly.

> ⚠️ **This path is not hardware-tested, and the bootloader rewrite at `0x0` is
> the one irreversible step.** If it is interrupted (power loss mid-write) the
> device has no valid loader and needs UART recovery. Keep your full 8 MB UART
> backup as the guaranteed fallback.

### Older modules need a one-time UART reflash

Modules flashed with an early build (via ESPConnect) run an ESP-IDF bootloader whose partition table sits at `0x8000` with the ESP-IDF-default geometry (`otadata@0xf000`, 2.5 MB OTA slots at `0x20000`/`0x2a0000`). This firmware reads its table at `0x10000` (the stock Shelly offset). Those two layouts are incompatible: `0x10000` is the **second `otadata` sector** on the old geometry, so an over-the-air update onto such a module boots once into the new image but reverts to the old one on the next reboot (the new slot is never marked valid because `otadata` is disturbed).

Therefore old modules must be brought onto the current layout with a **one-time UART reflash** of the full merged image (`tools/make_factory_bin_file.sh`, flash at offset `0x0` with *Erase entire flash before writing*). This writes the partition table at `0x10000` with the stock geometry, after which all further updates are OTA and persist across reboots. A full erase also clears the Matter fabrics, so re-commission the module afterwards (make a dashboard backup first if needed).

## Pin mapping

**Onboard Shelly 1 Gen4:**

| GPIO | Function |
|---|---|
| **GPIO4** | PCB button (active-low) |
| **GPIO5** | Relay output (active-high) |
| **GPIO10** | Pushbutton input / SW terminal |
| **GPIO15** | Status LED (active-low) |

**Shelly Plus Add-on** (via J6 connector):

| GPIO | Function |
|---|---|
| **GPIO9** | 1-Wire TX — DS18B20 commands via ISO7221A isolator |
| **GPIO16** | 1-Wire RX — DS18B20 responses via isolator |
| **GPIO17** | Analog IN — occupancy sensor (e.g. HLK-LD2410S) |
| **GPIO18** | Digital IN — TTP223 capacitive touch / add-on switch |

**J6 connector pinout** (1.27 mm pitch, 7-pin header on back of PCB):

| Pin | Function | GPIO | Notes |
|---|---|---|---|
| 1 | ESP_DBG_UART | GPIO18 | not used for flashing |
| 2 | TXD | GPIO16 | Shelly TXD → CP2102 RXD |
| 3 | RXD | GPIO17 | Shelly RXD ← CP2102 TXD |
| 4 | 3.3V | — | power supply (no 5V!) |
| 5 | RESET | EN | not needed for manual flashing |
| 6 | GPIO0 (BOOT) | GPIO0 | low at power-up → flash mode |
| 7 | GND | — | pin closest to `J6` silkscreen |

## Lua scripting API

### Input functions

| Function | Returns | Description |
|---|---|---|
| `input.button_event()` | string or nil | Last button event (see events table below) |
| `input.button_id()` | integer | Input that triggered the event: `0`=SW, `1`=Digital IN, `2`=PCB button, `3`=SW2 (2PM) |
| `input.sw()` | boolean | Current state of SW input (GPIO10) |
| `input.digital()` | boolean | Current state of Digital IN (GPIO18) |
| `input.device_btn()` | boolean | Current state of PCB button (GPIO4) |
| `input.analog()` | integer | Analog IN duty cycle 0–100 % (GPIO17) |
| `input.temperature()` | number | DS18B20 (Add-on) temperature in °C |
| `input.chip_temperature()` | number or nil | ESP32-C6 internal temperature in °C (all models) |

### Button events

| Event string | Description |
|---|---|
| `"short_press"` | Short press (< 500ms) |
| `"long_press_start"` | Long press started |
| `"long_press_stop"` | Long press released |
| `"double_press"` | Double press |
| `"short_long_start"` | Short press followed by long press started |
| `"short_long_stop"` | Short-long press released |
| `"contact_closed"` | Button/switch contact closed (pressed) |
| `"contact_open"` | Button/switch contact opened (released) |

### Input IDs

The SW / button / Digital-IN GPIOs depend on the selected model (see the
hardware table above). The `GPIO` column below lists the default **Shelly 1 Gen4**
pins; the 2PM-specific pins are noted separately.

| ID | Input | GPIO |
|---|---|---|
| `0` | SW (1st wall switch) | GPIO10 (1 Gen4) — **GPIO11 on 2PM** |
| `1` | Digital IN (add-on) | GPIO18 |
| `2` | PCB button (onboard) | GPIO4 |
| `3` | SW2 (2nd wall switch, 2PM only) | **GPIO10 on 2PM** |

### Output functions

The relay functions take an **optional 1-based channel** argument (`1` = relay 1,
`2` = relay 2 on the 2PM). When omitted, channel 1 is used.

| Function | Description |
|---|---|
| `output.relay_set(on)` | Set relay 1 on/off (`on` = boolean) |
| `output.relay_set(ch, on)` | Set relay `ch` (1 or 2) on/off |
| `output.relay(...)` | Alias for `output.relay_set` |
| `output.relay_toggle([ch])` | Toggle relay `ch` (default 1) |
| `output.relay_state([ch])` | Returns state of relay `ch` (default 1) as boolean |

> **Script migration note:** the relay API is now channel-indexed. Existing
> single-relay scripts using `output.relay_set(true)` / `output.relay_toggle()` /
> `output.relay_state()` **keep working unchanged** (they act on relay 1). Only
> 2PM scripts that need the second relay must pass a channel:
> `output.relay_set(2, true)`.

### Endpoint functions (client endpoints)

| Function | Description |
|---|---|
| `endpoint.command("toggle")` | Send OnOff Toggle to bound devices |
| `endpoint.command("on")` | Send OnOff On |
| `endpoint.command("off")` | Send OnOff Off |
| `endpoint.command("move_with_onoff", {up=bool, rate=N})` | Start dimming |
| `endpoint.command("stop")` | Stop dimming |
| `endpoint.command("color_temp_set", {mireds=N})` | Set color temperature |
| `endpoint.command("color_temp_move", {warmer=bool, rate=N})` | Start color temp change |
| `endpoint.command("color_temp_stop")` | Stop color temp change |

### Other functions

| Function | Description |
|---|---|
| `endpoint.set(attr, value)` | Set sensor attribute (for server endpoints) |
| `log(msg)` | Print to serial log |
| `timer.millis()` | Uptime in milliseconds |

## WiFi behavior

| State | WiFi | Thread/BLE | How to reach |
|---|---|---|---|
| **Factory reset** (no scripts) | ON — STA + AP | OFF | After flash or factory reset |
| **Configured** (scripts, no fabrics) | OFF | ON (BLE commissioning) | After configuring endpoints + reboot |
| **Commissioned** (normal) | OFF | ON (Thread active) | After commissioning |
| **6× press** (management) | ON — APSTA mode | Thread disabled | Press any button 6× rapidly |
| **WiFi mode** (management) | ON — STA or AP | Thread disabled | "Restart to WiFi mode" button (10 min, then back to Thread) |

## Status LED

The onboard status LED (GPIO15) indicates the device state:

| Pattern | Description |
|---|---|
| **Fast blink** (5 Hz) | Boot / initialization in progress, or OTA update active |
| **Slow blink** (1 Hz) | Not commissioned — waiting for BLE pairing |
| **Heartbeat** (short flash every 2s) | Normal operation — commissioned and online |
| **Off** | LED disabled or no pattern set |

During boot the LED blinks fast. After initialization it switches to heartbeat (if commissioned) or slow blink (if not yet commissioned).

## BENCH_MODE

Controls GPIO10 polarity and sensor initialization. Configurable at runtime via the management dashboard.

| BENCH_MODE | GPIO10 | Sensors | Use case |
|---|---|---|---|
| **0** | Active-high (230V optocoupler) | Active | Production |
| **1** (default) | Active-low + pull-up | Skipped (UART0 stays active) | Development |

## Build + flash

- **[INSTALL.md](INSTALL.md)** — Linux/macOS/WSL2 command-line setup with esp-matter and ESP-IDF
- **[INSTALL_VSCODE_WINDOWS.md](INSTALL_VSCODE_WINDOWS.md)** — VS Code on Windows 11 + WSL2

## File structure

```
shelly_gen4_matter_module/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── app_config.h        # pins, timings, BENCH_MODE
│   ├── app_main.cpp        # boot sequence, smart boot logic
│   ├── matter_device.cpp   # dynamic endpoint creation + command emit
│   ├── script_engine.c     # Lua 5.4 scripting engine
│   ├── hw_config.c/.h      # runtime hardware profile (1/Mini/1PM/2PM)
│   ├── button.c/.h         # button driver (wall switches, PCB button, gestures)
│   ├── relay.c/.h          # relay GPIO control (1 or 2 channels)
│   ├── power_meter.c/.h    # BL0942 driver (1PM Gen4)
│   ├── ade7953.c/.h        # ADE7953 dual-channel driver (2PM Gen4)
│   ├── sensors.c/.h        # DS18B20 + analog occupancy
│   ├── ota.c/.h            # WiFi runtime, management dashboard, OTA
│   ├── status_led.c/.h     # LED patterns
│   ├── secrets.h           # compile-time WiFi credentials (gitignored)
│   └── CHIPProjectConfig.h # vendor/product name overrides
├── components/lua/         # Lua 5.4 as ESP-IDF component
├── tools/
│   ├── make-matter-ota.py       # build Matter OTA image (.ota) — see Firmware updates
│   ├── make-webui-ota-zip.py    # legacy: Shelly 1.x web-UI OTA zip (unsupported)
│   ├── make_factory_bin_file.sh # merge binaries for UART/ESPConnect flashing
│   ├── create_matter_cluster_group.py  # set up multicast group + bindings
│   └── shelly-overview.sh       # clickable HTML overview of modules via mDNS
├── SCRIPTS.md              # example Lua scripts
├── INSTALL.md
└── INSTALL_VSCODE_WINDOWS.md
```

## Known limitations

- **Test vendor ID**: firmware uses vendor ID 0xFFF1. For Google/Apple Home publication a CSA vendor ID is required.
- **Test DAC**: for production, provision real Device Attestation Certificates in the NVS `chip-factory` namespace. For local HA usage the test DAC works fine.

## License

Espressif esp-matter and connectedhomeip: Apache 2.0. Lua: MIT. This custom code: MIT.
