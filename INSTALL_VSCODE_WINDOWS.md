# Installation via Visual Studio Code on Windows 11 (WSL2) — `shelly_gen4_matter_module`

Step-by-step guide to build and flash this Matter-over-Thread firmware project from **Visual Studio Code on Windows 11**, with the build running in **WSL2 (Ubuntu 22.04)**.

> **Why WSL2 and not native Windows?** ESP-Matter (Espressif's SDK) and connectedhomeip (Matter stack) support **only Linux and macOS**. On Windows, `wsl --install` is the officially recommended route — as confirmed by Espressif's own documentation. Native Windows builds via `install.bat` do not exist.

## Architecture

```
Windows 11
├── VS Code (Windows side, with Remote WSL extension)
└── WSL2 (Ubuntu 22.04)
    ├── ESP-IDF (toolchain for ESP32-C6)
    ├── ESP-Matter SDK (Matter implementation)
    └── USB via usbipd-win → /dev/ttyUSB0 or /dev/ttyACM0
```

For the `chip-tool` binding commands (after pairing) you use your Home Assistant Matter Server container — it already has `chip-tool` built in. WSL does not need to build `chip-tool` itself.

---

## 0. Prerequisites

- **OS**: Windows 11 (21H2 or newer)
- **Hardware**: minimum 16 GB RAM, 40 GB free on the Windows C:\ drive
- **BIOS**: Virtualization (Intel VT-x or AMD-V) enabled
- **Software**: VS Code on Windows (not in WSL)
- **Stable internet connection** for downloads (~5 GB of repos + tools)

---

## 1. Install WSL2 + Ubuntu 22.04

Open PowerShell as **Administrator** → run:

```powershell
wsl --install -d Ubuntu-22.04
```

This activates WSL features, downloads the WSL2 kernel, and installs Ubuntu 22.04.

⚠️ **Restart Windows** after installation. The Windows features are only active after a reboot.

After the reboot Ubuntu starts automatically. Choose a **UNIX username** (lowercase, no spaces) and a **password** (sudo password, not your Windows login).

### Check kernel version

In Ubuntu:
```bash
uname -a
```

Version must be **5.10.60.1 or higher**. If not, in PowerShell:
```powershell
wsl --upgrade
```

### Troubleshooting WSL installation

- **Error `0x80370102`** → Virtualization is disabled in BIOS. Reboot, enter BIOS (Del/F2 during boot), enable **Intel VT-x** or **AMD-V**, save & reboot.
- **WSL Stopped** after a Windows reboot → normal. Open Ubuntu via start menu, or run `wsl` in PowerShell to start.

---

## 2. Install usbipd-win (USB forwarding to WSL2)

WSL2 does not see USB ports by default. `usbipd-win` is the official bridge.

PowerShell as Administrator:
```powershell
winget install usbipd
```

Restart the PowerShell terminal after install so `usbipd` is on PATH.

The actual coupling of the UART adapter to WSL is done later in step 8.

---

## 3. Install Linux dependencies

In Ubuntu (start menu → Ubuntu 22.04 LTS, or `wsl` in PowerShell):

```bash
sudo apt update && sudo apt upgrade -y

sudo apt install -y git wget curl flex bison gperf \
    python3 python3-pip python3-venv python3-setuptools \
    python3.11 python3.11-venv python3.11-dev \
    cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0 libglib2.0-dev libavahi-client-dev \
    libreadline-dev libevent-dev pkg-config
```

Ubuntu 22.04 ships Python 3.10, but esp-matter `release/v1.6` pins
`mobly==1.13`, which is published as `requires-python: >=3.11`. Hence the
explicit `python3.11` above; step 5 points the bootstrap at it.

Add yourself to the `dialout` group for access to `/dev/ttyUSB*`:
```bash
sudo usermod -aG dialout $USER
```
Close and reopen the Ubuntu terminal (or `exit` + `wsl`) so the group change takes effect.

---

## 4. Install ESP-IDF

```bash
mkdir -p ~/esp
cd ~/esp
git clone --recursive -b v5.5.5 https://github.com/espressif/esp-idf.git

cd ~/esp/esp-idf
./install.sh esp32c6

source ./export.sh
idf.py --version
```

Expected output:
```
ESP-IDF v5.5.5
```

Duration: 10-15 minutes.

---

## 5. Install ESP-Matter

ESP-Matter has many submodules via `connectedhomeip`. We use a **stable release branch**; `main` is the ongoing effort towards the next Matter version.

```bash
cd ~/esp
source esp-idf/export.sh

git clone --depth 1 -b release/v1.6 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1
```

> Check [esp-matter releases](https://github.com/espressif/esp-matter/releases) for the most recent stable branch.

### Fetch platform-specific submodules

```bash
cd ~/esp/esp-matter/connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ~/esp/esp-matter
```

### Install

```bash
./install.sh
```

⚠️ Takes **15-45 minutes** depending on internet speed and hardware.

### If install.sh fails with `mobly` / `ResolutionImpossible`

The bootstrap used Python 3.10. Point it at 3.11 and rebuild the environment:
```bash
cd ~/esp/esp-matter/connectedhomeip/connectedhomeip
rm -rf .environment
cd ~/esp/esp-matter
PW_BOOTSTRAP_PYTHON=/usr/bin/python3.11 ./install.sh
```

This discards the half-populated Python venv and bootstraps a new one on 3.11.

---

## 6. Enable ccache + auto-environment

First Matter build takes > 1 hour. With ccache, subsequent builds are **5-10× faster**.

```bash
echo 'export IDF_CCACHE_ENABLE=1' >> ~/.bashrc
echo 'source ~/esp/esp-idf/export.sh' >> ~/.bashrc
echo 'source ~/esp/esp-matter/export.sh' >> ~/.bashrc
source ~/.bashrc
```

Now `idf.py` and all ESP-Matter env vars are automatically available in every new Ubuntu terminal.

---

## 7. Set up VS Code

### 7a. Install extensions (Windows side)

In VS Code (`Ctrl+Shift+X`):
- **WSL** (`ms-vscode-remote.remote-wsl`) — by Microsoft
- **Espressif IDF** (`espressif.esp-idf-extension`) — by Espressif

### 7b. Copy project to WSL

Place the project zip in Windows (e.g. `Downloads`), then in Ubuntu:

```bash
mkdir -p ~/projects
cd ~/projects
cp "/mnt/c/Users/<your-windows-user>/Downloads/shelly_gen4_matter_module.zip" .
unzip shelly_gen4_matter_module.zip
cd shelly_gen4_matter_module
```

⚠️ Project must be in WSL filesystem (`~/projects/...`), **not** in `/mnt/c/...`. Builds from `/mnt/c/` are 5-10× slower due to cross-FS overhead.

### 7c. Open project in VS Code (WSL mode)

From the same Ubuntu terminal in the project folder:
```bash
code .
```

VS Code opens automatically in **Remote-WSL mode**. The badge **`WSL: Ubuntu-22.04`** appears in the bottom left — this confirms everything runs in Ubuntu.

> This `code .` command is **the simplest route**. No `WSL: Connect to WSL` wizards, no manual folder mappings.

### 7d. Activate Espressif IDF extension in WSL context

`Ctrl+Shift+X` → search **"Espressif IDF"** → click **"Install in WSL: Ubuntu-22.04"** if that button appears.

The command palette (`F1`) then shows ESP-IDF commands like **"Open ESP-IDF Terminal"**, **"Build Project"**, etc.

> The **"Configure ESP-IDF extension"** wizard is not strictly necessary — because we already source ESP-IDF + tools in `~/.bashrc`, every ESP-IDF terminal in VS Code automatically finds the correct path.

If the wizard is needed (e.g. config validation fails): choose **"Use existing setup"** with:
- ESP-IDF dir: `/home/<user>/esp/esp-idf`
- Tools dir: `/home/<user>/.espressif`
- Python venv: `/home/<user>/.espressif/python_env/idf5.5_py3.10_env/bin/python`

---

## 8. Connect ESP32-C6 / Shelly via UART

Open the Shelly 1 Gen4 housing and connect a USB-UART adapter (CP2102) to the **J6 connector** (7-pin row). Full procedure + canonical pinout is in [`INSTALL.md` chapter 4](INSTALL.md#4-uart-flashing-via-the-j6-connector-on-the-back) — here only the summary:

| CP2102 | Shelly J6 pin | Purpose |
|---|---|---|
| 3.3V (**not 5V**) | Pin 4 | Power |
| GND | Pin 7 (next to `J6` silkscreen) | Ground |
| RXD | Pin 2 (Shelly TXD) | UART data |
| TXD | Pin 3 (Shelly RXD) | UART data |
| GND (bridge, for flash mode) | Pin 6 (GPIO0 / BOOT) | During power-up |

⚠️ **Important**: Pin 4 is **3.3V only** — never 5V. Pin numbering starts at the pin farthest from the `J6` label (pin 1 = ESP_DBG_UART) and ends at `J6` (pin 7 = GND).

Plug the USB adapter into a Windows USB port.

### 8a. View available USB devices

PowerShell as **Administrator**:
```powershell
usbipd list
```

Example output:
```
BUSID  VID:PID    DEVICE                                  STATE
3-2    10c4:ea60  Silicon Labs CP210x USB to UART Bridge  Not shared
```

### 8b. Forward dev board to WSL2

```powershell
# One-time (admin rights required):
usbipd bind --busid 3-2

# Attach to WSL (WSL must be running):
usbipd attach --wsl --busid 3-2
```

⚠️ After every `wsl --shutdown` or Windows reboot, `usbipd attach --wsl` must be run again.

### 8c. Verify in Ubuntu

```bash
lsusb
ls /dev/ttyUSB* /dev/ttyACM*
```

Expected: `/dev/ttyUSB0` visible.

| Chip / Board | USB bridge | Port in WSL |
|---|---|---|
| ESP32-C3 / C6 / H2 (native USB) | Built-in JTAG | `/dev/ttyACM0` |
| ESP32 / ESP32-S3 (WROOM) | CP2102 / CH340 | `/dev/ttyUSB0` |
| ESP32-DevKitC | CP2104 | `/dev/ttyUSB0` |

Shelly 1 Gen4 with external USB-UART adapter (CP2102): `/dev/ttyUSB0`.

---

## 9. Build and flash the project

In VS Code (Remote-WSL active), open an **ESP-IDF Terminal** via `F1` → "ESP-IDF: Open ESP-IDF Terminal".

```bash
# Set target (one-time per project)
idf.py set-target esp32c6

# Build
idf.py build

# Flash (first time: bridge Pin 6 (GPIO0) ↔ Pin 7 (GND) during power-up for flash mode)
idf.py -p /dev/ttyUSB0 flash

# Monitor
idf.py -p /dev/ttyUSB0 monitor

# Or all in one command:
idf.py -p /dev/ttyUSB0 flash monitor
```

`Ctrl+]` to exit the monitor.

**First build takes 25-45 min**. Subsequent builds with ccache: 1-3 min.

Or via VS Code buttons (after correctly configured extension):
- `F1` → "ESP-IDF: Build Project" (`Ctrl+E B`)
- `F1` → "ESP-IDF: Flash Device" (`Ctrl+E F`)
- `F1` → "ESP-IDF: Monitor Device" (`Ctrl+E M`)
- `F1` → "ESP-IDF: Build, Flash and Start a Monitor" (`Ctrl+E D`)

---

## 10. Read Setup Pin Code / QR Code

In the serial monitor you'll see on first boot:
```
CHIP:SVR: SetupQRCode: [MT:U9VJ142C00KA0648G00]
CHIP:SVR: Copy/paste the below URL in a browser to see the QR Code:
CHIP:SVR: https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3AU9VJ142C00KA0648G00
CHIP:SVR: Manual pairing code: [34970112332]
```

Open the URL → show QR code to your HA Matter Server UI (Settings → Devices & Services → Add → Matter → scan code).

Default test passcode: `20202021` (defined in `sdkconfig.defaults`).

---

## 11. Binding setup via chip-tool (on your HA host)

Your `chip-tool` runs in the Home Assistant Matter Server container — not in WSL. For binding write:

Via HA Advanced SSH addon:
```bash
docker exec -it addon_core_matter_server bash
# Inside container:
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":<bulb-node-id>,"endpoint":1,"cluster":6},
    {"fabricIndex":1,"node":<bulb-node-id>,"endpoint":1,"cluster":8}]' \
  <switch-node-id> 1
```

Replace `<bulb-node-id>` and `<switch-node-id>` with the node IDs that HA shows for your KAJPLATS and Shelly.

Cluster 6 = OnOff, cluster 8 = LevelControl.

---

## 12. OTA — firmware updates

The firmware supports two OTA paths:

### WiFi OTA (primary, ready to use)
1. `idf.py build` produces new `build/shelly_gen4_matter_module.bin`
2. Copy this file to HA's `/config/www/`
3. Click **10× rapidly** on your System 55 pushbutton on the Shelly
4. First time: phone → SoftAP `shelly-ota-XXXXXX` → `http://192.168.4.1/` → enter SSID/pass/URL
5. Subsequent times: 10× clicks suffice — credentials are saved

### Matter OTA Provider (optional)
The Matter OTA Requestor is activated in the firmware (`esp_matter_ota_requestor_init()` in `matter_start()`). To use this you need to run an external `chip-ota-provider-app` (see connectedhomeip examples). HA does not offer a built-in provider today; WiFi OTA remains the practical default.

---

## 13. Daily workflow

After one-time installation:

```bash
# 1. Start Ubuntu (start menu → Ubuntu 22.04 LTS)
# 2. Open project
cd ~/projects/shelly_gen4_matter_module
code .

# 3. Forward ESP32 (PowerShell admin)
usbipd attach --wsl --busid <your-busid>

# 4. In VS Code ESP-IDF Terminal:
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## 14. Common problems

| Problem | Cause | Fix |
|---|---|---|
| `wsl: command not found` / WSL not installed | WSL feature off | `wsl --install -d Ubuntu-22.04` as admin, reboot |
| WSL error `0x80370102` | Virtualization off in BIOS | BIOS → enable VT-x / AMD-V |
| `WSL State: Stopped` | Normal after reboot | `wsl` or start menu Ubuntu |
| `install.sh` fails on `mobly` / `ResolutionImpossible` | Pigweed bootstrapped on Python 3.10; `mobly==1.13` needs 3.11+ | `rm -rf connectedhomeip/connectedhomeip/.environment && PW_BOOTSTRAP_PYTHON=/usr/bin/python3.11 ./install.sh` |
| `idf.py: command not found` | Env not sourced | `source ~/esp/esp-idf/export.sh` (or add to `~/.bashrc`) |
| `permission denied` on `/dev/ttyUSB0` | User not in dialout group | `sudo usermod -aG dialout $USER` + reopen terminal |
| USB port not visible in WSL | Forgot `usbipd attach` after reboot | PowerShell admin: `usbipd attach --wsl --busid <ID>` |
| ESP-IDF extension says "config not valid" | Auto-detect fails | Skip wizard, add `source ~/esp/esp-idf/export.sh` to `~/.bashrc` and reload VS Code |
| Build crashes with `out-of-memory` | WSL2 default RAM too low | Create `%USERPROFILE%\.wslconfig`: `[wsl2]` + `memory=8GB` + `processors=4`. Then `wsl --shutdown` and restart |
| Slow build (>1 hour 2nd time) | Ccache not enabled | `export IDF_CCACHE_ENABLE=1` permanently in `~/.bashrc` |
| Build from `/mnt/c/` extremely slow | Cross-FS overhead | Move project to `~/projects/` |

---

## 15. Known build issues with older esp-matter release branches

These project templates are already fixed; listed here for reference for anyone setting up their own Matter projects on v1.4.

### 15.1 `static_assert: Wi-Fi network endpoint id and Thread network endpoint id should not be the same`

**Cause:** both `CONFIG_ENABLE_WIFI_STATION=y` and `CONFIG_OPENTHREAD_ENABLED=y` active → Matter tries to create two Network Commissioning Cluster instances on endpoint 0.

**Fix** (in `sdkconfig.defaults`):
```
CONFIG_ENABLE_MATTER_OVER_THREAD=y
CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=y
CONFIG_ENABLE_WIFI_STATION=n
CONFIG_ENABLE_WIFI_AP=n
```

Master toggle `CONFIG_ENABLE_MATTER_OVER_THREAD=y` gates all WiFi-Matter paths out.

### 15.2 `fatal error: esp_matter.h: No such file or directory`

**Cause:** main component missing `esp_matter` in `PRIV_REQUIRES`.

**Fix** (`main/CMakeLists.txt`):
```cmake
PRIV_REQUIRES
    ...
    chip
    esp_matter
```

### 15.3 `'ChipDevicePlatformEvent' does not name a type` + `static_cast` from `ConnectivityManager*` → `ConnectivityManagerImpl*` fails

**Cause:** platform target defines are missing. esp-matter's GN build normally passes these through to CMake, but on v1.4 release branch with Thread-only configs this sometimes leaks.

**Fix** (root `CMakeLists.txt`, after `project()`):
```cmake
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
```

And in `main/CMakeLists.txt`:
```cmake
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
```

`CHIP_HAVE_CONFIG_H` triggers the correct platform config selection in connectedhomeip headers. Pattern taken from `$ESP_MATTER_PATH/examples/light_switch/CMakeLists.txt`.

### 15.4 Partition table `does not fit in configured flash size 2MB`

**Cause:** Shelly 1 Gen4 has 8 MB flash, ESP-IDF default is 2 MB.

**Fix** (`sdkconfig.defaults`):
```
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
```

### 15.5 API drift v1.4 (code level)

Code written against older Matter API fails on v1.4 with:

| Old | New in v1.4 |
|---|---|
| `EMBER_MULTICAST_BINDING` | `MATTER_MULTICAST_BINDING` |
| `EMBER_UNICAST_BINDING` | `MATTER_UNICAST_BINDING` |
| `ESP_MATTER_NONE_FEATURE_MAP` | `ESP_MATTER_NONE_FEATURE_ID` |
| `chip::BindingManager::Params` | `chip::BindingManagerInitParams` (top-level, with extra `mStorage` field) |
| `cmd.optionsMask = 0` (int → BitMask) | Omit — default init is already an empty BitMask |
| `InvokeCommandRequest(... , void(*)(void*, ...), void(*)(void*, CHIP_ERROR))` | Typed lambdas: `[](const ConcreteCommandPath&, const StatusIB&, const auto& response){}` for success, `[](CHIP_ERROR){}` for error |

Plus `#include <app/clusters/bindings/BindingManager.h>` (BindingManager class moved away from `binding-table.h`).

---

## 16. Next steps

See:
- [`README.md`](README.md) — architecture, Matter device types, binding explanation
- [`INSTALL.md`](INSTALL.md) — CLI fallback workflow (pure Linux/macOS, no VS Code)
- Espressif's [ESP-Matter Programming Guide](https://docs.espressif.com/projects/esp-matter/en/latest/) for deeper SDK info
