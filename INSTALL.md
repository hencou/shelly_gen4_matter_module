# Installation and build guide — `shelly_gen4_matter_module`

⚠️ First build takes **20–45 minutes** because connectedhomeip must compile. After that, incremental builds with ccache take ~1-3 min.

> For Windows users: use [`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md) (WSL2 + VS Code is the officially recommended route). Native Windows is not supported by esp-matter.

## 1. ESP-IDF v5.4.1

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive -b v5.4.1 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
. ./export.sh           # every new shell session
```

## 2. Clone esp-matter (one-time, ~3 GB with submodules)

```bash
cd ~/esp
# Use a branch with Matter 1.5 support (e.g. main or release/v1.5+)
git clone --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1

# Platform-specific submodules (esp32 + linux for chip-tool)
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ~/esp/esp-matter

./install.sh
. ./export.sh           # sets $ESP_MATTER_PATH + adds chip-tool to PATH
```

⚠️ If `install.sh` fails with `mobly` / `ResolutionImpossible`:
```bash
rm -rf connectedhomeip/connectedhomeip/.environment
./install.sh
```

⚠️ The `connectedhomeip/` submodule alone is ~2 GB. Make sure you have enough free space (>5 GB for build artifacts).

## 3. Build the project

```bash
# Enable ccache for faster rebuild cycles
export IDF_CCACHE_ENABLE=1

# Load both environments (every shell session)
. ~/esp/esp-idf/export.sh
. ~/esp/esp-matter/export.sh

cd ~/projects/shelly_gen4_matter_module
idf.py set-target esp32c6
idf.py menuconfig     # adjust GPIOs if needed
idf.py build          # first time 20-45 min, then 1-3 min
```

Tip: add both `source ...export.sh` lines + `IDF_CCACHE_ENABLE=1` to your `~/.bashrc` so they load automatically in every new terminal.

For known build issues, see **section 15 in [`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md)**.

In `menuconfig`:
- **Shelly 1 Gen4 Matter Switch configuration** → GPIOs
- **Component config → CHIP Device Layer** → leave defaults (Thread enabled, BLE pairing enabled)
- **Partition Table → Custom** → already set correctly via `sdkconfig.defaults`

Expected output: `build/shelly_gen4_matter_module.bin` (~1.6-1.8 MB).

## 4. UART flashing via the J6 connector on the back

The Shelly 1 Gen4 has **7 holes in a row** on the back — this is the **J6 connector** (silkscreen label `J6`) of the integrated ESP32-C6 module. This is the same row of pins where a Shelly Plus Add-on connects, but it can also be used to flash the ESP32-C6.

You can also install this firmware **directly from the stock Shelly firmware over WiFi**, without opening the device, using a Shelly Web UI OTA zip (`python3 tools/make-webui-ota-zip.py`, uploaded via the Shelly device web interface — see [README.md → Firmware updates](README.md#firmware-updates)).

⚠️ **However, the Web UI OTA route cannot back up the stock Shelly firmware** — it only writes the new app. UART flashing (this section) is the **only** way to read out and save the original 8 MB stock image first. If you might ever want to return to stock, do the UART backup below **before** flashing anything. Restoring stock later also requires UART.

> **Note**: the J6 pinout below was verified on hardware revision `v0.1.2`
> (printed on the PCB).

### J6 Pinout

Numbering runs **from the pin farthest from the J6 label (pin 1) to the pin right next to the label (pin 7 = GND)**.

| Pin | Function | GPIO | Notes |
|---|---|---|---|
| 1 | **ESP_DBG_UART** | GPIO18 (Digital IN) | not used for flashing |
| 2 | **TXD** | GPIO16 (UART0 TX // 1-Wire RX) | Shelly TXD → CP2102 RXD |
| 3 | **RXD** | GPIO17 (UART0 RX // Analog IN) | Shelly RXD ← CP2102 TXD |
| 4 | **3.3V** | -- | power supply (3.3V only — **no 5V**) |
| 5 | **RESET** | EN | EN — not needed for manual flashing |
| 6 | **GPIO0 (BOOT)** | GPIO0 | low at power-up → flash mode |
| 7 | **GND** | -- | ground — pin right next to the `J6` silkscreen |

⚠️ **Pin 7 = GND is your orientation anchor**: use a multimeter on continuity, find the pin that beeps against the metal shield of the ESP module or a known GND trace pad — that is pin 7. Count back from there to pin 1 in the opposite direction.

### What you need

- **CP2102 USB-UART adapter** with separate 3.3V output (this guide was tested on CP2102). FTDI FT232RL / CH340G also work as long as they can supply 3.3V and operate at 3.3V logic level.
- 1.27 mm 7-pin to 2.54 mm Dupont cable or adapter board (the J6 holes are 1.27 mm pitch). Solid-core Cat 5e/6 wires also fit in the square holes in a pinch, but are hard to hold steady.
- A short jumper wire or pushbutton for the **Pin 6 ↔ Pin 7 (GPIO0 ↔ GND)** bridge during flash mode entry.
- A Chromium browser (Chrome / Edge) for the ESPConnect web flasher, or `esptool.py` / `idf.py` via CLI.

### ⚠️ Safety — read first

- **Never connect 230V to the Shelly while flashing.** The programming header is **not** galvanically isolated from the relay circuit. USB-UART + mains simultaneously = electrocution risk for you, broken Shelly, broken USB port, broken laptop. Completely disconnect the Shelly from the wall outlet / junction box before connecting anything.
- **Never apply 5V to pin 4.** The pin is directly connected to the 3.3V rail of the ESP32-C6. 5V will destroy the module.
- **Bench test note**: when powering via pin 4 (3.3V from USB-UART), the relay does **not** click audibly — the relay coil needs 230V. Firmware, commissioning, GPIO, LED, and Matter pairing all work fully. If you don't hear a click on a short press: that's normal in bench mode, not a bug.

### Wiring

USB-UART **not plugged into the PC yet** during wiring. Connect as follows:

```
CP2102 pin  ←→  Shelly J6
──────────────────────────────────
GND         →  Pin 7  (GND, right next to J6 label)
3.3V        →  Pin 4  (3.3V — NOT 5V)
RXD         →  Pin 2  (Shelly TXD)
TXD         →  Pin 3  (Shelly RXD)

Do not connect on CP2102:  5V, DTR
Do not connect on Shelly:  Pin 1 (DBG), Pin 5 (RESET)

For flash mode (extra):
GND         →  Pin 6  (GPIO0 BOOT, jumper or pushbutton)
```

TX/RX crossing is already handled in the table above — CP2102 **RXD** goes to Shelly **TXD** (pin 2), CP2102 **TXD** to Shelly **RXD** (pin 3).

### Procedure — entering flash mode

The Shelly boots into ROM bootloader when GPIO0 (pin 6) is low at power-up.

1. Connect wiring (USB-UART not plugged into PC yet).
2. **Bridge Pin 6 (GPIO0) and Pin 7 (GND)** — jumper or pushbutton held down.
3. **Plug USB-UART into PC** → Shelly gets 3.3V via pin 4, boots directly into flash mode.
4. Bridge can stay connected during flashing, or released after ~1 second — both work once the bootloader is running.
5. **After successful flashing**: remove bridge before the next power-on, otherwise Shelly boots into flash mode again instead of your firmware.

### WSL2-specific — forwarding USB-UART

From PowerShell as admin:

```powershell
usbipd list                       # note the BUSID of the CP2102
usbipd bind --busid <BUSID>       # one-time per adapter
usbipd attach --wsl --busid <BUSID>
```

Afterwards the device appears in WSL as `/dev/ttyUSB0` (or `/dev/ttyACM0` for some chipsets — check with `ls /dev/tty*`).

## 5. Back up stock firmware + flash

Two routes — pick one.

> ⚠️ **Important about flashing via J6**: the J6 connector has **no DTR/RTS wiring**, so esptool's auto-reset does not work. With `idf.py flash` (Route B) you must re-enter flash mode **before each command** by bridging Pin 6 ↔ Pin 7 and re-plugging the USB-UART. That's cumbersome. **Route A (merged binary + ESPConnect) does everything in a single flash action and is recommended.**

### Route A — Merged binary + ESPConnect (browser, one-shot flash)

Our build produces 4 separate binaries that each need to be at their own offset (bootloader 0x0, partition-table 0x8000, otadata 0xf000, app 0x20000). ESPConnect can only write one file at one offset, so we merge first.

**Step 1 — build merged bin (one-time per build):**

```bash
. ~/esp/esp-idf/export.sh
cd ~/projects/shelly_gen4_matter_module
idf.py build      # ensures all 4 binaries are up to date

esptool.py --chip esp32c6 merge_bin \
    -o shelly_gen4_matter_module_merged.bin \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0      build/bootloader/bootloader.bin \
    0x8000   build/partition_table/partition-table.bin \
    0xf000   build/ota_data_initial.bin \
    0x20000  build/shelly_gen4_matter_module.bin

# Copy to Windows side for ESPConnect (replace <USERNAME>)
cp shelly_gen4_matter_module_merged.bin /mnt/c/Users/<USERNAME>/Downloads/
```

Tip: put those last three commands in a script `tools/make-merged.sh` for later iterations.

**Step 2 — flash via ESPConnect:**

1. **Unplug USB-UART** from PC, **bridge Pin 6 ↔ Pin 7 (GPIO0 ↔ GND)**, plug USB-UART back in → Shelly in ROM bootloader. Bridge can stay or be removed, both work.
2. Open [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect/) in Chrome / Edge.
3. **Connect** → select CP2102 serial port. Baud rate **115200** (lower = more stable for large writes).
4. ESPConnect shows chip info including **MAC address** — note for backup naming.
5. **Optional but recommended**: **Flash Tools → Download Flash Backup** → 8 MB stock image (`shelly-1-gen4-stock-AABBCCDDEEFF.bin`). Takes 5–10 min. Verify file ≈ 8,388,608 bytes. Keep safe — only way back to factory.
6. **Flash Tools → Flash Firmware**:
   - File: `shelly_gen4_matter_module_merged.bin`
   - Offset: `0x0`
   - **Erase entire flash before writing**: on
   - Click **Flash**. ~1-2 min.
7. **Disconnect** in ESPConnect.
8. Unplug USB-UART, **remove Pin 6 ↔ Pin 7 bridge** (critical — otherwise Shelly boots into flash mode again), plug USB-UART back in → Shelly now runs your Matter firmware and opens BLE pairing.

### Route B — esptool / `idf.py` (CLI, for developers)

Requires re-entering flash mode between **every** esptool call (erase, flash, etc.) because DTR/RTS is not wired on J6.

```bash
# Back up stock (one-time — keep safe!)
# Prerequisite: bridge Pin 6 ↔ Pin 7, re-plug USB
esptool.py --chip esp32c6 -p /dev/ttyUSB0 --baud 460800 \
    --before no_reset --after no_reset \
    read_flash 0x0 0x800000 shelly_stock_backup.bin

# Erase + flash Matter firmware
# Prerequisite: bridge Pin 6 ↔ Pin 7, re-plug USB
idf.py -p /dev/ttyUSB0 erase-flash

# Prerequisite: bridge Pin 6 ↔ Pin 7, re-plug USB
idf.py -p /dev/ttyUSB0 flash

# Monitor (remove bridge, re-plug USB)
idf.py -p /dev/ttyUSB0 monitor
```

Restoring stock can always be done later (also with manual flash mode entry):

```bash
esptool.py --chip esp32c6 -p /dev/ttyUSB0 write_flash 0x0 shelly_stock_backup.bin
```

After flashing: **remove Pin 6 ↔ Pin 7 bridge**, then power-cycle. Otherwise Shelly boots into flash mode again.

In the log you should see something like:
```
I (... ) app: Not commissioned, no scripts — WiFi setup mode (BLE off)
I (... ) ota: wifi_runtime: APSTA mode — AP 'shelly-cfg-XXXX' + STA '...'
I (... ) chip[DL]: Device Configuration:
I (... ) chip[DL]:   Setup Pin Code: 20202021
I (... ) chip[DL]:   Setup Discriminator: 3840 (0xF00)
I (... ) chip[DL]:   Manual Pairing Code: [...]
I (... ) chip[DL]:   QR Code: [...]
```

Note the **QR Code** or **Manual Pairing Code** — use these to add the device in HA.

## 6. Prepare Thread network

You already have a Google TV Streamer 4K = working TBR (Google Home fabric).
For HA Matter Server, HA must also be connected to that same Thread network:

**Option A — Shared Thread Credentials**
HA Matter Server automatically picks up Thread credentials from Google TV via the Apple/Google/HA Thread Credentials API (Android 14+ / iOS 16.4+).

**Option B — HA Connect ZBT-2 as secondary TBR**
Plug a ZBT-2 into the HA host → HA becomes secondary TBR → joins the same Thread mesh.

Check in HA → Settings → Devices & Services → Thread → you should see at least one dataset.

## 7. Pairing in HA Matter Server

1. HA → Settings → Devices & Services → "Add Integration" → Matter Server → "Add device".
2. Scan the QR code from the UART log or enter the Manual Pairing Code.
3. Wait 30-90 s. It pairs via BLE, receives Thread credentials, joins Thread, and then appears as "Shelly 1 Gen4 Matter Switch" with the endpoints you configured via the Scripts page.

If pairing fails:
- Check that the Shelly booted less than 5 minutes ago (BLE pairing window).
- Press **6× rapid clicks** on any button → enables WiFi management dashboard. From there you can factory reset to clear Matter NVS and reopen the pairing window.

## 8. Pair a KAJPLATS bulb in Matter mode

KAJPLATS is dual-stack (Zigbee + Matter-over-Thread). For Matter:

1. Screw in the bulb, lamp turns on.
2. On the IKEA box there is a QR code for Matter setup, or use the default code from IKEA's website.
3. HA → Matter → Add device → scan QR.
4. Bulb joins Thread, appears in HA as KAJPLATS light.

Note the Matter node-ID of the bulb (visible in HA device info).

## 9. Binding setup with chip-tool

Endpoints are now dynamic — the endpoint IDs depend on your script slot configuration. Check the boot log or HA device info to find the correct endpoint IDs.

From your laptop:

```bash
. ~/esp/esp-matter/export.sh    # adds chip-tool to PATH

# Replace with your own node-IDs and endpoint IDs
SWITCH=0x0123456789ABCDEF       # Shelly node-ID
BULB=0x00112233AABBCCDD         # Target lamp node-ID
SWITCH_EP=1                     # Endpoint of your client script slot
BULB_EP=1                       # Endpoint on the lamp

# Bind client endpoint to the lamp (OnOff + LevelControl + ColorControl)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":'$BULB_EP',"cluster":6},
    {"fabricIndex":1,"node":'$BULB',"endpoint":'$BULB_EP',"cluster":8},
    {"fabricIndex":1,"node":'$BULB',"endpoint":'$BULB_EP',"cluster":768}]' \
  $SWITCH $SWITCH_EP
```

Press the button → the lamp should respond directly, **without HA in the path**.

## 10. OTA — firmware updates without UART

1. Build new firmware: `idf.py build`. Place `build/shelly_gen4_matter_module.bin` on a web server reachable from your WiFi network (e.g. HA `/config/www/`).
2. Press **6× rapidly** on any button → WiFi management dashboard activates.
3. Navigate to the WiFi tab in the dashboard → enter SSID + password + OTA URL.
4. The device downloads and flashes the new firmware, then reboots.
5. For subsequent OTA updates: 6× press suffices — WiFi credentials are saved in NVS.

## 11. Rollback to stock Shelly firmware

```bash
esptool.py -p /dev/ttyUSB0 write_flash 0x0 shelly_stock_backup.bin
```

## 12. Troubleshooting

| Symptom | Cause / solution |
|---|---|
| Build fails with "esp_matter.h not found" | `$ESP_MATTER_PATH` not set. Run `. ~/esp/esp-matter/export.sh`. |
| Build OOM (out of memory) | connectedhomeip is heavy. Min 8 GB RAM recommended, or `idf.py -j2 build` for more sequential builds. |
| Device does not appear in HA after flash | Check BLE pairing window: log should show "BLE advertising started". Reboot device to reopen window. |
| Thread join fails | No active Thread credentials in fabric. Check HA → Thread → see if dataset exists. |
| Binding command doesn't work | Check `chip-tool binding read binding $SWITCH 1` — should show your new entries. Bulb must have ACL allowing switch to write (HA Matter Server does this automatically during commissioning). |
| Lamp responds slowly (>1s) | Thread mesh can recover via rebooting Google TV Streamer; check `chip-tool thread show-state $SWITCH 0`. |
| Crash at boot after OTA | Bootloader rollback active (see sdkconfig). It automatically falls back to the previous slot. Check your build. |
| Long-press dimming doesn't work | Binding must include **cluster 8** (LevelControl), not just cluster 6. |
| WiFi AP not visible after 6× press | Ensure 6 rapid clicks within ~3s. If Thread is active (commissioned device), Thread is automatically disabled to free the radio. Try again after reboot. |
| `esptool` timeout on `/dev/ttyUSB0` | Pin 6 (GPIO0) was not low at power-on. Bridge Pin 6 ↔ Pin 7 (GND) again and re-plug USB-UART. See chapter 4. |
| Shelly boots into flash mode again after flash | Pin 6 ↔ Pin 7 bridge is still connected. Remove the jumper and power-cycle. |
| Accidentally applied 5V to Pin 4 | Module is most likely dead. No rescue possible — replace only. Always use 3.3V. |
