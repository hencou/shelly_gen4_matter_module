# shelly_gen4_matter_module

**Custom Matter-over-Thread firmware** for the **Shelly Gen4** line (ESP32-C6) — one image for the Shelly 1, 1PM and 2PM Gen4 — with **Lua scripting** for fully configurable endpoints.

## Disclaimer

> ⚠️ **Read this before you flash anything.**
>
> Installing this firmware **voids your Shelly warranty**, and Shelly cannot
> provide technical support for a device running third-party code. It can remove
> the factory keys that enable Shelly Cloud and official OTA updates. Treat
> flashing as **one-way** unless you keep the full-chip backup you make *before*
> flashing. Incorrect flashing can **brick your device**, so always back up your
> original firmware before proceeding if reversibility is important to you. You
> assume all responsibility for any damage, data loss, or device failure.
>
> This project is not affiliated with Shelly or Espressif Systems.

## Features

- **Dynamic Matter endpoints** — no hard-coded endpoints. Configure via the web management dashboard
- **Lua 5.4 scripting** — write custom button/relay/sensor logic per endpoint slot (up to 8 slots)
- **Matter 1.6** compatible — works with Home Assistant, Google Home, Apple Home
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

- [esp-matter](https://github.com/espressif/esp-matter) `release/v1.6` (**Matter 1.6**)
- [connectedhomeip](https://github.com/project-chip/connectedhomeip) (as submodule within esp-matter)
- ESP-IDF v5.5.5
- Lua 5.4 (compiled as component)

## Target hardware

One firmware image supports four Gen4 models (all ESP32-C6, 8 MB flash). Select
the model on the management dashboard (**Hardware → Device Type**); the choice is
stored in NVS and applied on the next boot. The correct GPIO mapping is then used
for the relay(s), wall-switch input(s), onboard button and status LED.

| Model | Relay | Switch | Button | Status LED | Add-on (Digital IN) | Power meter |
|---|---|---|---|---|---|---|
| **Shelly 1 Gen4** (default) | GPIO5 | GPIO10 | GPIO4 | GPIO15 | yes (GPIO18) | — |
| **Shelly 1 Mini Gen4** | GPIO10 | GPIO12 | GPIO22 | GPIO5 | — | — |
| **Shelly 1PM Gen4** | GPIO4 | GPIO10 | GPIO1 | GPIO11 | yes (GPIO12) | BL0942 (UART1 GPIO7 + GPIO6, 9600 baud) |
| **Shelly 2PM Gen4** | GPIO5 + GPIO3 | GPIO11 + GPIO10 | GPIO12 | GPIO18 | yes (GPIO1) | ADE7953 dual-channel (IRQ=GPIO19, I2C SDA=GPIO6 SCL=GPIO7) |

See [STOCK_GPIO.md](STOCK_GPIO.md) for the evidence per field. The Add-on Analog IN
(GPIO17) and 1-Wire (GPIO16 in / GPIO9 out) are identical on every model; only
Digital IN moves.

The polarity is the same on every model: relays are **active-high**, status LEDs
**active-low**, the onboard button and Add-on Digital IN **active-low** (internal
pull-up), and the wall-switch inputs **active-high** in normal operation (see
[BENCH_MODE](#bench_mode) for the development exception).

> ℹ️ **Shelly 1 Mini Gen4 is supported.** Since installing straight from the stock Shelly web UI now
> works (see [Firmware updates](#firmware-updates)), the Mini can be flashed
> without opening it. The Mini has **no** Shelly Plus Add-on connector, so the
> add-on inputs are unavailable on it.

> ⚠️ **Test status:** only the **Shelly 1 Gen4** has been verified on real
> hardware. The other three profiles match the stock firmware pin-for-pin but
> have **not** been hardware-tested. The BL0942 and ADE7953 scaling constants
> are placeholders and **must** be calibrated against a known load on real
> hardware before the reported voltage/current/power values are trustworthy.

> ⚠️ **ADE7953 I2C pins are the one unverified value.** Stock reads the I2C
> SDA/SCL pins from the device configuration in NVS instead of hardcoding them.
> Only the ADE7953 IRQ (GPIO19) is confirmed. The 2PM relay/switch pins themselves
>  *are* confirmed: the ESPHome device DB
> (https://devices.esphome.io/devices/shelly-plus-2pm-gen-4/) contradicts itself
> on GPIO5/GPIO3/GPIO11/GPIO10, and stock agrees with its YAML config (relays on
> GPIO5/GPIO3, switches on GPIO11/GPIO10).

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
| Shelly Plus Add-on | DS18B20 (TX=GPIO9/RX=GPIO16) + Digital IN (GPIO18 on the 1 Gen4) + Analog IN (GPIO17) |
| Thread Border Router | Google TV Streamer 4K (or any Thread BR) |
| Matter controller | Home Assistant Matter Server, Google Home, Apple Home |
| Commissioning | HA Matter Server UI or `chip-tool` |

## Setup procedure

### 1. First flash

There are two ways to install the firmware for the first time.

**Option A — from the stock Shelly web UI (no UART, no opening the device):**
build the web-UI zip package and upload it through the stock Shelly device page
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

> Keep a full backup before flashing (see the warning under
> [Firmware updates](#firmware-updates)).

**Option B — UART flash (see [INSTALL.md](INSTALL.md)):** open the device and
wire a USB-UART adapter to the J6 connector. This installs the ESP-IDF bootloader
together with the partition table and app, and is also the way to make a full
8 MB backup of the stock Shelly firmware for a guaranteed return to stock.

After either first install, **all** further updates are over the air (Matter OTA
/ dashboard `.bin` upload, or a fresh web-UI package) — no UART needed.

### 2. Factory reset → commissioning mode

After flashing (or a factory reset via the web interface) the module comes up in
**BLE commissioning mode** straight away — no WiFi, no setup step first:

1. Open Home Assistant → Settings → Devices & Services → Matter → "Add device"
2. Enter setup code: **34970112332** (default, configurable in `sdkconfig.defaults`)
3. HA Matter Server pairs via BLE and provisions Thread credentials
4. After ~30-60s the device appears in HA

A module that is already commissioned skips this and goes straight to Thread.

### 3. Configure endpoints via the dashboard

Endpoints do **not** have to exist before commissioning. Open the management
dashboard (over Thread, or via the 10-minute WiFi window — see below) and go to
the **Scripts** tab:

1. Set a **name**, **endpoint type**, **trigger**, and **Lua script** for each slot you need
2. Click **Save** for each slot
3. Click **Reboot** on the Scripts page

The new endpoints appear in Home Assistant by themselves; if not, re-interview
the device (Matter integration → device → *Reconfigure*/interview).

See [SCRIPTS.md](SCRIPTS.md) for example scripts.

### 4. Normal operation

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

The fallback server (**Hardware** tab → *SRP fallback server*, `srp_mode`) runs an
on-device SRP / DNS-SD server, but only while the mesh has no border router; it
yields the moment a real one appears, and only one module takes the role
(election on RLOC16, lowest wins). The server needs the Router or Leader role, so
a module that is only an End Device never runs it.

How it is built: `CONFIG_OPENTHREAD_BORDER_ROUTER` stays **off** — turning it on
links the prebuilt `libopenthread_br.a` plus the ESP border-router glue, whose
MeshCoP mDNS publisher needs an infra/backbone interface this device does not
have, and the module then hangs right after Thread attach on every reboot after
commissioning. The SRP/DNS-SD server sources (`srp_server.cpp`,
`dnssd_server.cpp`) are part of *every* FTD build though — only the enabling
macros sit inside ESP-IDF's `#if CONFIG_OPENTHREAD_BORDER_ROUTER` block. So the
server is enabled through a custom OpenThread header
([`main/openthread_custom_config.h`](main/openthread_custom_config.h)), which
needs no border-router glue at all.

> The fallback has no advertising proxy: it fixes discovery *inside* the mesh
> (a switch resolving a bound lamp), not discovery of the mesh from your LAN. For
> that, add a real Thread border router with an advertising proxy (e.g. a Home
> Assistant OpenThread Border Router on a ZBT-1/SkyConnect dongle).

#### Switch to WiFi for faster management

**Enable WiFi 10 min** on the dashboard (**WiFi & OTA** tab) joins WiFi as a
**station** next to a running Thread network, **without a reboot**, and switches
WiFi off again after 10 minutes. Pressing it again extends the window; pressing
it while the window is open closes it early.

The physical shortcut does exactly the same: **press any button 6× rapidly**
(within 2.5 seconds). Use that when the dashboard is unreachable over Thread.

**Apply** on that tab only stores SSID, password, hostname and firmware URL — no
reboot. The next window uses them, so a wrong SSID costs a toggle instead of a
restart. **Restart** next to it reboots the device on request.

Both work regardless of commissioning status. Without saved WiFi credentials —
or when they do not connect within a minute — the window switches to an **open
SoftAP** `shelly-cfg-XXXXXX` with the dashboard on `http://192.168.4.1/` for the
remainder of the ten minutes. That is how you reach a module that has never been
commissioned, and it keeps a module with stale credentials reachable when Thread
is not configured either.

##### What temporary WiFi costs while it is open

WiFi and 802.15.4 share one radio on the ESP32-C6, and Espressif documents only
**one** stable combination: WiFi *station* next to a Thread **End Device**
(SoftAP next to a Thread Router is listed as unsupported, and SoftAP next to an
End Device only as *limited* once a client is connected). The 10-minute window
therefore:

- starts WiFi as STA (SoftAP when there are no credentials, or when they fail);
- keeps Thread up, but gives up the **router role** (`otThreadSetRouterEligible(false)`): no routing for other nodes, no children;
- in the SoftAP fallback additionally makes Thread a **sleepy child** (`mRxOnWhenIdle=false`, 3 s parent poll). A station has an AP buffering frames for it while the radio serves 802.15.4, a SoftAP has nothing of the kind: with 802.15.4 receiving all the time, clients associate but never get a DHCP lease. Thread stays attached, but mesh traffic is as slow as the poll period until the window closes. Sleepy mode also drops the FTD role and full network data in the same link mode, because OpenThread refuses rx-off-when-idle on a full Thread device. If the stack still refuses, Thread goes **down** for the rest of the window and comes back at teardown: an unreachable module is worse than a Thread outage that ends by itself;
- re-arms the coexistence arbiter (`esp_coex_wifi_i154_enable()`) before **every** `esp_wifi_start()`, because stopping WiFi hands the radio back to 802.15.4 — the SoftAP fallback would otherwise run without airtime;
- stands down the **SRP fallback server**, which requires Router/Leader;
- shares the radio, so expect more Thread packet loss while WiFi is busy.

All of that is restored automatically when the window closes — no reboot. If
Thread detaches while the window is open, the Thread watchdog closes the window
immediately: Thread wins over temporary WiFi — unless the SoftAP fallback took
Thread down deliberately, in which case the detached state is expected. The current state is visible on the
**Hardware** tab (*Temporary WiFi*).

> Not hardware-verified: coexistence stability only shows up under real WiFi
> traffic over time. Check that the lamps keep switching over Thread while the
> dashboard is served over WiFi, and that the router role returns after the
> window closes.

### Backup & restore

Via the management dashboard → **Backup** tab:
- **Download Backup** — exports all settings as a JSON file: WiFi credentials + all 8 script slot configurations (name, type, trigger, period, Lua code)
- **Restore Backup** — upload a previously downloaded JSON backup to restore all settings. The device reboots automatically after restore.

### Factory reset

Via the web management dashboard → **Factory Reset** button. This wipes:
- All NVS data (WiFi credentials, script configurations, bench mode)
- All Matter fabrics and commissioning data (NVS namespaces)

After factory reset the module reboots into BLE commissioning mode (step 2).

Before wiping, both **Factory Reset** and **Commission Mode** hand the SRP host
name and its services back to the SRP server, key lease included. The wipe takes
the SRP client key with it, and without that hand-back the server keeps the old
registration for the rest of its key lease (days) and rejects the same host name
under the fresh key with `SRP update error: domain name or RRset is duplicated`
— visible during commissioning as a module that stays hard to discover. If the
server does not confirm within 3 s the reset continues anyway; restarting the
border router (or `ot-ctl srp server disable` / `enable`) clears the stale claim.

## Firmware updates

Once the custom firmware is running you can update it three ways. They all flash the **same** application binary (`build/shelly_gen4_matter_module.bin`) — they only differ in transport, and all use standard ESP-IDF OTA slot selection with bootloader rollback (a bad image is automatically reverted to the previous slot).

> ⚠️ **Keep a full UART backup as your guaranteed way back.** The management
> page can flash an original Shelly firmware package back onto the device (see
> [Return to stock](#4-return-to-stock-shelly-firmware) below), and that path is
> verified on a Shelly 1 Gen4 — but it depends on the package matching your
> device. The one route that always
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

The image embeds the vendor/product ID and software version; the device only accepts an image with a higher software version than it currently runs.

For each release bump the version in **two** places — the build fails if they disagree:

| Where | Value | Reported as |
|---|---|---|
| `PROJECT_VER` in `CMakeLists.txt` | `1.6.4` | `SoftwareVersionString` and the version on the management dashboard; `PROJECT_VER_NUMBER` (`major*10000 + minor*100 + patch`) is derived from it and reported as `SoftwareVersion` |
| `main/CHIPProjectConfig.h` | `10604` / `"1.6.4"` | the version the `.ota` image is tagged with |

Both numbers must match: a controller compares `SoftwareVersion` (a number), not the string. If the `.ota` advertises a number the running firmware does not report, Home Assistant shows a permanent "update available" for the firmware it already runs — displayed as `1.6.4 (10604)` next to installed version `1.6.4`.

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

### 4. Return to stock Shelly firmware

The management dashboard (**Backup** tab) can flash an **original Shelly firmware package** back onto the module — the same `.zip` the stock web UI consumes. Download the package matching the model on your device label from the [community firmware archive](https://archive.shelly-tools.de/) (e.g. `S4SW-001X16EU` = Shelly 1 Gen4), then upload it under *Return to stock Shelly firmware*.

The stock app cannot run under our ESP-IDF bootloader (it needs the Shelly OS loader and an `SH0S` boot state), so the module is made **byte-for-byte stock** again. The firmware first writes the stock **app** to the inactive slot and the stock **filesystem**, and verifies their SHA-256 — nothing outside that inactive slot is touched until this succeeds. It then restores, in order, the stock **boot state** (`otadata`), the stock **partition table** (`0x10000`), points the `SH0S` boot-select at the slot the stock app landed in, and finally rewrites the stock **bootloader** (Shelly OS loader) at `0x0`. Every write is verified by read-back. The factory `shelly` partition is never touched. The units are not flash-encrypted, so the plaintext images from the package reproduce the stock layout exactly.

The restore also erases our `nvs` partition, so the device comes back up as a
factory-fresh stock unit and has to be set up again from scratch.

> ✅ Verified on a **Shelly 1 Gen4**: the stock `S4SW-001X16EU` package restored
> over the air boots the Shelly OS loader and the stock app again.

> ⚠️ **The bootloader rewrite at `0x0` is the one irreversible step.** If it is
> interrupted (power loss mid-write) the device has no valid loader and needs
> UART recovery. Keep your full 8 MB UART backup as the guaranteed fallback.

## Pin mapping

**Onboard Shelly 1 Gen4:**

| GPIO | Function |
|---|---|
| **GPIO4** | PCB button — **active-low**, internal pull-up |
| **GPIO5** | Relay output — **active-high** (high = relay closed) |
| **GPIO10** | Pushbutton input / SW terminal — **active-high**, no internal pull (mains-referenced input circuit). In bench mode it becomes active-low with a pull-up so a plain button to GND works, see [BENCH_MODE](#bench_mode) |
| **GPIO15** | Status LED — **active-low** (low = LED on) |

**Shelly Plus Add-on** (via J6 connector):

| GPIO | Function |
|---|---|
| **GPIO9** | 1-Wire TX — DS18B20 commands via ISO7221A isolator. **Active-low** open-drain signalling (idle high); the isolator does not invert, so driving GPIO9 low pulls the bus low |
| **GPIO16** | 1-Wire RX — DS18B20 responses via isolator. **Active-low**, idle high; a presence pulse reads as low |
| **GPIO17** | Analog IN — occupancy sensor (e.g. HLK-LD2410S). **Active-high** PWM duty cycle with internal pull-down; ≥25 % duty (≈2.5 V on the 0–10 V scale) counts as occupied |
| **GPIO18** | Digital IN — TTP223 capacitive touch / add-on switch. **Active-low**, internal pull-up (touch/contact active pulls the pin low). The Add-on terminal itself is active-low too (Shelly specifies −15 V…0.5 V = true, 2.5 V…15 V = false), so the isolator passes the level through uninverted. The stock firmware's "invert digital input" setting does not exist here — invert it in your Lua script if you need the opposite sense |

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
| `input.digital()` | boolean | Current state of Digital IN (GPIO18 on the 1 Gen4) |
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
| `1` | Digital IN (add-on) | GPIO18 (1 Gen4) — **GPIO12 on 1PM, GPIO1 on 2PM** |
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
| **Not commissioned** | OFF | ON (BLE commissioning) | After flash or factory reset |
| **Commissioned** (normal) | OFF | ON (Thread active) | Dashboard over Thread |
| **Temporary WiFi** (management) | ON — STA, SoftAP when STA fails | Thread active as End Device (no router role, no SRP fallback; sleepy child during SoftAP, or Thread down if sleepy is refused) | "Enable WiFi 10 min" button or 6× press — no reboot, restores itself after 10 min |

## Status LED

The onboard status LED (GPIO15) indicates the device state:

| Pattern | Description |
|---|---|
| **Fast blink** (5 Hz) | Boot / initialization in progress, or OTA update active |
| **Slow blink** (1 Hz) | Not commissioned — waiting for BLE pairing |
| **Heartbeat** (short flash every 2s) | Normal operation — commissioned and online |
| **Off** | LED disabled or no pattern set |

During boot the LED blinks fast. After initialization it switches to heartbeat (if commissioned) or slow blink (if not yet commissioned).

## Unicast binding reachability

A unicast binding sends over a cached CASE session, and that session holds the
IPv6 address the peer had when the session was created. Thread addresses are not
stable — a border router restart hands out a new on-mesh prefix, and a parent
change gives a node a new RLOC — so the address can go stale while the session
still looks healthy. Sending then goes nowhere and MRP retries for ~35 s.

Three mechanisms keep that off the critical path:

| Mechanism | What it does |
|---|---|
| Invoke response timeout (1.5 s) | Fixed, in firmware. After a timeout the session is evicted and the command is retried once over a fresh CASE session — which does a new DNS-SD lookup. Skipped when the peer acknowledged the first attempt, because `Toggle` is not idempotent. |
| Thread network-data watch | Automatic. When the set of on-mesh prefixes changes, every cached session to a bound peer is evicted right away, because all peer addresses just expired. |
| Binding keepalive | Configurable. Reads `ClusterRevision` from each bound peer's bound cluster every N seconds and drops the session when that read fails, so the failure is discovered in the background instead of on a button press. |

The keepalive interval is set on the Hardware tab or over HTTP; `0` disables it,
the minimum is 60 s. Default is 600 s.

```
curl -X POST http://<ip>/api/keepalive -H 'Content-Type: application/json' -d '{"seconds":600}'
```

Cost is one round trip of ~100–150 bytes per bound peer per interval, and the
`ReadClient` is released once the answer arrives — no subscription is kept, on
either side. A read is used rather than a subscription on purpose: a Matter
device only has to support three subscriptions per fabric, and the controller
needs those itself.

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
│   ├── make-webui-ota-zip.py    # build Shelly Stock web-UI OTA zip
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
