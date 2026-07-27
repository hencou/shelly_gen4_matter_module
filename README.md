# shelly_gen4_matter_module

**Custom Matter-over-Thread firmware** for the **Shelly Gen4** line (ESP32-C6) — one image for the Shelly 1, 1 Mini, 1PM and 2PM Gen4 — with **Lua scripting** for fully configurable endpoints.

## Features

- **Dynamic Matter endpoints** — no hard-coded endpoints. Configure via the web management dashboard
- **Lua 5.4 scripting** — write custom button/relay/sensor logic per endpoint slot (up to 8 slots)
- **Matter 1.5** compatible — works with Home Assistant, Google Home, Apple Home
- **Thread + WiFi** — Thread for Matter communication, WiFi for management/OTA
- **Smart boot** — auto-detects factory reset vs configured vs commissioned state
- **WiFi management dashboard** — configure scripts, WiFi, endpoints, backup/restore
- **Multiple update paths** — Matter OTA over Thread, Shelly Web UI OTA (also directly from stock Shelly firmware), and `.bin` upload via the dashboard. See [Firmware updates](#firmware-updates).

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
- ESP-IDF v5.4.1
- Lua 5.4 (compiled as component)

## Target hardware

One firmware image supports four Gen4 models (all ESP32-C6, 8 MB flash). Select
the model on the management dashboard (**Hardware → Device Type**); the choice is
stored in NVS and applied on the next boot. The correct GPIO mapping is then used
for the relay(s), wall-switch input(s), onboard button and status LED.

| Model | Relay | Switch | Button | Status LED | Add-on | Power meter |
|---|---|---|---|---|---|---|
| **Shelly 1 Gen4** (default) | GPIO5 | GPIO10 | GPIO4 | GPIO15 | yes | — |
| **Shelly 1 Mini Gen4** | GPIO10 | GPIO12 | GPIO22 | GPIO5 | no | — |
| **Shelly 1PM Gen4** | GPIO4 | GPIO10 | GPIO1 | GPIO0 | yes | BL0942 (UART1 TX=GPIO6 RX=GPIO7, 9600 baud) |
| **Shelly 2PM Gen4** | GPIO5 + GPIO3 | GPIO11 + GPIO10 | GPIO12 | GPIO0 | yes | ADE7953 dual-channel (I2C SDA=GPIO6 SCL=GPIO7, IRQ=GPIO19) |

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
  the 1 Gen4, 1PM Gen4 and 2PM Gen4, and **not** on the Mini.
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

Two options for getting this firmware onto a stock Shelly 1 Gen4:

- **UART flash** (see [INSTALL.md](INSTALL.md)) — the classic route. Requires opening the device and wiring a USB-UART adapter to the J6 connector. This is the **only** way to make a backup of the stock Shelly firmware first (see the warning under [Firmware updates](#firmware-updates)).
- **Shelly Web UI OTA** — install directly from the stock Shelly firmware over WiFi, no UART needed. Upload a Web UI OTA zip through the Shelly device web interface. See [Firmware updates](#firmware-updates) for how to build the zip and the important caveat about backups.

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

To access the management dashboard: **press any button 6× rapidly** (within 2.5 seconds). This disables Thread and starts WiFi in APSTA mode:
- AP always available: `shelly-cfg-XXXX` on `192.168.4.1`
- STA connects to your router (if credentials are saved)

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

Once the custom firmware is running you can update it three ways. All three flash the **same** application binary (`build/shelly_gen4_matter_module.bin`) — they only differ in transport.

> ⚠️ **No stock Shelly backup via OTA.** Installing this firmware directly from the stock Shelly firmware (Shelly Web UI OTA) does **not** and cannot make a backup of the original Shelly firmware — the OTA process only writes the new app and never reads the existing flash. The **only** way to back up the stock firmware is over **UART** with a tool such as [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect/) or `esptool.py read_flash` (see [INSTALL.md](INSTALL.md)). If you may ever want to return to stock, make that UART backup **before** you flash — after the OTA it is too late.

### 1. Matter OTA (over Thread)

Update over the existing Thread/Matter connection — no WiFi or cabling needed. Build the `.ota` image and serve it from a Matter OTA provider (e.g. Home Assistant):

```bash
idf.py build
python3 tools/make-matter-ota.py      # → shelly-gen4-matter-module-v<version>.ota
```

The image embeds the vendor/product ID and software version; the device only accepts an image with a higher software version than it currently runs. Bump `CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION[_STRING]` in `main/CHIPProjectConfig.h` for each release.

> Tip: `make-matter-ota.py` just wraps whatever is in `build/`. Always `idf.py build` first (use `idf.py fullclean` to force a genuine recompile) and confirm the `.ota` is fresh — a stale `build/` produces a stale `.ota`.

### 2. Shelly Web UI OTA (zip) — also from stock firmware

Build a zip that the Shelly device web interface accepts as a local firmware update. This works both **from the stock Shelly firmware** (initial install, no UART) and **from this firmware** (updates):

```bash
idf.py build
python3 tools/make-webui-ota-zip.py   # → shelly-gen4-matter-module-v<version>-ota.zip
```

Upload the zip via the Shelly device's own web interface (local firmware update). The zip keeps the existing bootloader (its bundled bootloader is marked `min_version 0.0.0`, so nothing at offset `0x0` is overwritten) and uses the stock partition layout (PT at `0x10000`). The firmware migrates the partition table automatically on first boot where needed.

### 3. `.bin` upload via the management dashboard

For modules already running this firmware: open the management dashboard (**6× rapid button press** → WiFi), go to the **WiFi & OTA** tab, and either provide a firmware URL or upload `build/shelly_gen4_matter_module.bin` directly. The device flashes the inactive OTA slot and reboots into it.

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
| **WiFi persistent** (setting) | ON — APSTA mode | ON (coexistence) | Toggle in Hardware tab |
| **WiFi persistent + TBR** | ON — APSTA + TBR | ON (border router) | Toggle both in Hardware tab |

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
│   ├── make-webui-ota-zip.py    # build Shelly Web UI OTA zip (stock-compatible)
│   ├── make_factory_bin_file.sh # merge binaries for UART/ESPConnect flashing
│   └── create_matter_cluster_group.py  # set up multicast group + bindings
├── SCRIPTS.md              # example Lua scripts
├── INSTALL.md
└── INSTALL_VSCODE_WINDOWS.md
```

## Known limitations

- **Test vendor ID**: firmware uses vendor ID 0xFFF1. For Google/Apple Home publication a CSA vendor ID is required.
- **Test DAC**: for production, provision real Device Attestation Certificates in the NVS `chip-factory` namespace. For local HA usage the test DAC works fine.
- **WiFi + Thread coexistence**: ESP32-C6 has one 2.4 GHz radio shared via TDM. When WiFi persistent mode is OFF, Thread is disabled when WiFi is activated via 6× press and resumes on reboot. When WiFi persistent mode is ON, both coexist via software coexistence.

## License

Espressif esp-matter and connectedhomeip: Apache 2.0. Lua: MIT. This custom code: MIT.
