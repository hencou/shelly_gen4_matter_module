# Example Lua Scripts

This file contains ready-to-use Lua scripts for common use cases.
Copy-paste these into the **Scripts** tab of the management dashboard.

## Table of contents

- [1. Toggle (short press on/off)](#1-toggle-short-press-onoff)
- [2. Momentary / doordruk relay](#2-momentary--doordruk-relay)
- [3. State-follow switch](#3-state-follow-switch)
- [4. Full lamp control (toggle, dim, color)](#4-full-lamp-control-toggle-dim-color)
- [5. Full lamp control with direction toggle](#5-full-lamp-control-with-direction-toggle)
- [6. Temperature sensor (DS18B20)](#6-temperature-sensor-ds18b20)
- [7. Occupancy sensor (analog IN)](#7-occupancy-sensor-analog-in)
- [8. Multi-input script (different behavior per button)](#8-multi-input-script-different-behavior-per-button)
- [9. Relay toggle on short press](#9-relay-toggle-on-short-press)
- [10. LDR light-dependent lamp control](#10-ldr-light-dependent-lamp-control)

---

## 1. Toggle (short press on/off)

Sends a toggle command to bound devices on short press.

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | On button event |

```lua
function run()
  local evt = input.button_event()
  if evt == "short_press" then
    endpoint.command("toggle")
    log("toggle")
  end
end
```

---

## 2. Momentary / doordruk relay

Relay turns on while button is held, turns off when released.

| Setting | Value |
|---|---|
| Endpoint Type | Relay (OnOff Light server) |
| Trigger | On button event |

```lua
function run()
  local evt = input.button_event()
  if evt == "contact_closed" then
    output.relay_set(true)
    log("relay ON (pressed)")
  elseif evt == "contact_open" then
    output.relay_set(false)
    log("relay OFF (released)")
  end
end
```

---

## 3. State-follow switch

For a maintained/toggle switch: On when closed, Off when opened.
Sends commands to bound devices (e.g. a lamp via Matter binding).

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | On button event |

```lua
function run()
  local evt = input.button_event()
  if evt == "contact_closed" then
    endpoint.command("on")
    log("switch ON")
  elseif evt == "contact_open" then
    endpoint.command("off")
    log("switch OFF")
  end
end
```

---

## 4. Full lamp control (toggle, dim, color)

Complete lamp control with all gestures. Fixed dim/color direction.

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | On button event |

| Gesture | Action |
|---|---|
| Short press | Toggle on/off |
| Long press | Dim up (hold to dim) |
| Long press release | Stop dimming |
| Double press | Reset color to 2700K (warm white) |
| Short + long press | Change color temperature (warmer) |
| Short + long release | Stop color change |

```lua
function run()
  local evt = input.button_event()
  if evt == "short_press" then
    endpoint.command("toggle")
    log("toggle")
  elseif evt == "long_press_start" then
    endpoint.command("move_with_onoff", {up=true, rate=50})
    log("dim up")
  elseif evt == "long_press_stop" then
    endpoint.command("stop")
    log("dim stop")
  elseif evt == "double_press" then
    endpoint.command("color_temp_set", {mireds=370})
    log("reset to 2700K")
  elseif evt == "short_long_start" then
    endpoint.command("color_temp_move", {warmer=true, rate=50})
    log("color warmer")
  elseif evt == "short_long_stop" then
    endpoint.command("color_temp_stop")
    log("color stop")
  end
end
```

---

## 5. Full lamp control with direction toggle

Same as above, but dim direction and color direction alternate each time.

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | On button event |

```lua
local dim_up = true
local color_warmer = true

function run()
  local evt = input.button_event()
  if evt == "short_press" then
    endpoint.command("toggle")
    log("toggle")
  elseif evt == "long_press_start" then
    endpoint.command("move_with_onoff", {up=dim_up, rate=50})
    log("dim " .. (dim_up and "up" or "down"))
  elseif evt == "long_press_stop" then
    endpoint.command("stop")
    dim_up = not dim_up
    log("dim stop, next: " .. (dim_up and "up" or "down"))
  elseif evt == "double_press" then
    endpoint.command("color_temp_set", {mireds=370})
    log("reset to 2700K")
  elseif evt == "short_long_start" then
    endpoint.command("color_temp_move", {warmer=color_warmer, rate=50})
    log("color " .. (color_warmer and "warmer" or "cooler"))
  elseif evt == "short_long_stop" then
    endpoint.command("color_temp_stop")
    color_warmer = not color_warmer
    log("color stop, next: " .. (color_warmer and "warmer" or "cooler"))
  end
end
```

---

## 6. Temperature sensor (DS18B20)

Reads the DS18B20 temperature sensor and reports the value to Home Assistant
via the Matter Temperature Measurement cluster.

| Setting | Value |
|---|---|
| Endpoint Type | Temperature Sensor |
| Trigger | Periodic |
| Period | 10000 (10 seconds) |

The `input.temperature()` function returns the temperature in °C as a float.
The `endpoint.set("measured_value", N)` expects the value in **centi-°C** (100ths of a degree),
which is the Matter standard for temperature attributes (e.g. 2250 = 22.50°C).

```lua
function run()
  local temp = input.temperature()
  local centi = math.floor(temp * 100)
  endpoint.set("measured_value", centi)
  log("temp: " .. temp .. " C (" .. centi .. " centi-C)")
end
```

To expose the **Shelly's internal (ESP32-C6) temperature** instead of the Add-on
DS18B20, use `input.chip_temperature()` — this works on every model (also the
plain 1 Gen4 without Add-on). It returns `nil` if the sensor is unavailable:

```lua
function run()
  local temp = input.chip_temperature()
  if temp ~= nil then
    endpoint.set("measured_value", math.floor(temp * 100))
    log("chip temp: " .. temp .. " C")
  end
end
```

---

## 7. Occupancy sensor (analog IN)

Reads the analog input duty cycle (0–100 %) and reports occupancy state.
Useful with the HLK-LD2410S mmWave sensor connected to the Shelly Plus Add-on.

| Setting | Value |
|---|---|
| Endpoint Type | Occupancy Sensor |
| Trigger | Periodic |
| Period | 1000 (1 second) |

The duty cycle threshold (25 % ≈ 2.5 V on a 0–10 V scale) determines
when the sensor reports "occupied".

```lua
local threshold = 25

function run()
  local duty = input.analog()
  local occupied = duty >= threshold
  endpoint.set("occupied", occupied)
  log("duty=" .. duty .. "% occupied=" .. tostring(occupied))
end
```

### Contact sensor (switch / digital input state)

To expose the raw state of a wall switch or digital input as a binary sensor in
HA, use a **Contact Sensor (BooleanState)** slot and push the input state with
`endpoint.set("state", bool)`:

| Setting | Value |
|---|---|
| Endpoint Type | Contact Sensor (BooleanState server) |
| Trigger | On input change (or Periodic) |

```lua
function run()
  endpoint.set("state", input.sw())   -- or input.digital()
end
```

### Illuminance sensor (analog IN)

Expose a light level to Home Assistant via the Matter Illuminance Measurement
cluster. Use an **Illuminance Sensor** slot and push a value in **lux** with
`endpoint.set("illuminance", lux)` — the firmware encodes it to the Matter
`MeasuredValue` (`10000*log10(lux)+1`) for you; pass `0` for "dark/unknown".

| Setting | Value |
|---|---|
| Endpoint Type | Illuminance Sensor |
| Trigger | Periodic |
| Period | 5000 (5 seconds) |

If you have a real lux reading, pass it directly. The example below maps the
analog input (`0–100 %`, e.g. a 0–10 V light sensor) onto a 0–2000 lux range:

```lua
local max_lux = 2000

function run()
  local duty = input.analog()          -- 0..100 %
  local lux = duty / 100 * max_lux
  endpoint.set("illuminance", lux)
  log("duty=" .. duty .. "% -> " .. math.floor(lux) .. " lux")
end
```

---

## 8. Multi-input script (different behavior per button)

Use `input.button_id()` to distinguish which physical input triggered the event.

GPIOs are model-dependent (taken from the active hardware profile); the values below are the Shelly 1 Gen4 defaults.

| ID | Input | GPIO (1 Gen4) |
|---|---|---|
| `0` | SW (pushbutton terminal) | GPIO10 |
| `1` | Digital IN (add-on) | GPIO18 (Add-on models only) |
| `2` | PCB button (onboard) | GPIO4 |
| `3` | SW2 (2nd wall switch) | Shelly 2PM Gen4 only (GPIO10) |

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | On button event |

This example: SW toggles a lamp, PCB button toggles the relay, Digital IN state-follows.

```lua
function run()
  local evt = input.button_event()
  local id = input.button_id()

  if id == 0 then
    -- SW pushbutton: toggle bound lamp
    if evt == "short_press" then
      endpoint.command("toggle")
      log("SW: toggle lamp")
    end
  elseif id == 2 then
    -- PCB button: toggle relay
    if evt == "short_press" then
      output.relay_toggle()
      log("PCB: relay toggled")
    end
  elseif id == 1 then
    -- Digital IN: state-follow
    if evt == "contact_closed" then
      endpoint.command("on")
      log("DIN: on")
    elseif evt == "contact_open" then
      endpoint.command("off")
      log("DIN: off")
    end
  end
end
```

---

## 9. Relay toggle on short press

Simple relay toggle on any short press. The Matter OnOff attribute is
automatically updated so HA sees the state change in real-time.

| Setting | Value |
|---|---|
| Endpoint Type | Relay (OnOff Light server) |
| Trigger | On button event |

```lua
function run()
  local evt = input.button_event()
  --if evt == "short_press" then --or use contact_closed this will be faster
    if evt == "contact_closed" then
    output.relay_toggle()
    log("relay toggled via button")
  end
end
```

---

## 10. LDR light-dependent lamp control

Controls lamp brightness based on an LDR (light sensor) connected to the
analog input. When dark the lamp burns at minimum brightness; as ambient
light increases the lamp gets brighter; above 90 % the lamp turns off.

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | Periodic |
| Period | 5000 (5 seconds) |

`input.analog()` returns the LDR duty cycle 0–100 %.
Matter level range is 1–254 (1 = dimmest, 254 = full brightness).

```lua
local off_threshold = 90   -- above this: lamp off
local last_level = -1      -- track to avoid redundant commands
local was_off = false

function run()
  local duty = input.analog()

  if duty >= off_threshold then
    -- bright enough: turn lamp off
    if not was_off then
      endpoint.command("off")
      was_off = true
      last_level = -1
      log("duty=" .. duty .. "% -> lamp OFF")
    end
  else
    -- map 0..off_threshold → level 1..254
    local level = math.floor(duty * 253 / off_threshold) + 1
    if level > 254 then level = 254 end

    if was_off then
      endpoint.command("on")
      was_off = false
    end
    if level ~= last_level then
      endpoint.command("move_to_level", {level=level, transition=10})
      last_level = level
      log("duty=" .. duty .. "% -> level " .. level)
    end
  end
end
```

---

## Button events reference

| Event string | Description | Typical use |
|---|---|---|
| `"short_press"` | Short press (< 500ms) | Toggle on/off |
| `"long_press_start"` | Long press started | Start dimming |
| `"long_press_stop"` | Long press released | Stop dimming |
| `"double_press"` | Double press | Reset to default |
| `"short_long_start"` | Short then long press | Color temperature |
| `"short_long_stop"` | Short-long released | Stop color change |
| `"contact_closed"` | Button/switch closed | State-follow ON |
| `"contact_open"` | Button/switch opened | State-follow OFF |

## Script trigger types

| Trigger | When script runs | Use case |
|---|---|---|
| **On button event** | On every button press/release/gesture | Buttons, switches, relays |
| **Periodic** | Every N milliseconds (configurable) | Sensors (temperature, occupancy) |
| **On input change** | When any digital input changes state | Edge-triggered logic |

## Lua API quick reference

### Input
- `input.button_event()` → string or nil
- `input.button_id()` → integer (0=SW, 1=Digital IN, 2=PCB, 3=SW2 on 2PM)
- `input.sw()` → boolean
- `input.digital()` → boolean
- `input.device_btn()` → boolean
- `input.analog()` → integer (0–100 %)
- `input.temperature()` → number (°C, DS18B20 Add-on)
- `input.chip_temperature()` → number or nil (°C, ESP32-C6 internal sensor)

### Output
Relay functions take an optional 1-based channel (`1`=relay 1, `2`=relay 2 on the
2PM); omitting it targets relay 1, so existing single-relay scripts keep working.
- `output.relay_set(on)` / `output.relay_set(ch, on)` — set relay
- `output.relay(...)` — alias for `output.relay_set`
- `output.relay_toggle([ch])` — toggle relay
- `output.relay_state([ch])` → boolean

### Endpoint (client commands)
- `endpoint.command("toggle")`
- `endpoint.command("on")` / `endpoint.command("off")`
- `endpoint.command("move_with_onoff", {up=bool, rate=N})`
- `endpoint.command("move_to_level", {level=N, transition=T})` — N: 1–254, T: 1/10th seconds
- `endpoint.command("stop")`
- `endpoint.command("color_temp_set", {mireds=N})`
- `endpoint.command("color_temp_move", {warmer=bool, rate=N})`
- `endpoint.command("color_temp_stop")`

### Endpoint (sensor attributes)
- `endpoint.set("measured_value", centi_celsius)` — temperature (in 100ths of °C)
- `endpoint.set("illuminance", lux)` — illuminance (in lux; encoded to Matter internally)
- `endpoint.set("occupied", bool)` — occupancy
- `endpoint.set("state", bool)` — Contact Sensor (BooleanState) state
- `endpoint.set("on_off", bool)` — relay OnOff state

### Utilities
- `log(msg)` — print to serial log
- `timer.millis()` → integer (uptime in ms)
