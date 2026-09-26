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
- [11. Mode cycle on the SW input: on → off → LDR control](#11-mode-cycle-on-the-sw-input-on--off--ldr-control)

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
| Endpoint Type | OnOff State-Follow (client) |
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

The direction only alternates within one dimming session. After 10 seconds of
inactivity — or after a short press — it starts at "up" again. Without that
reset a first long press can arrive as "down" on a lamp that is already off,
which is a valid command that does nothing visible.

```lua
local IDLE_RESET_MS = 10000

local dim_up = true
local color_warmer = true
local last_dim_ms = 0

function run()
  local evt = input.button_event()
  if evt == "short_press" then
    endpoint.command("toggle")
    dim_up = true
    log("toggle")
  elseif evt == "long_press_start" then
    if timer.millis() - last_dim_ms > IDLE_RESET_MS then dim_up = true end
    endpoint.command("move_with_onoff", {up=dim_up, rate=50})
    log("dim " .. (dim_up and "up" or "down"))
  elseif evt == "long_press_stop" then
    endpoint.command("stop")
    dim_up = not dim_up
    last_dim_ms = timer.millis()
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
A sensor with a plain digital presence output (LD2410S OUT pin, PIR) can go on
the Digital IN instead and use `input.digital()` — see the contact sensor
example below.

> ⚠️ The Add-on sensor supply is limited to **10 mA**. Use the low-power
> LD2410**S** variant; the regular LD2410/LD2410B/LD2410C draw far more and
> cannot be powered from the Add-on. The Digital IN has a built-in pull-up and
> reads *true* when pulled to GND, so an active-high OUT pin reads inverted —
> invert it in Lua (`not input.digital()`) or use the sensor's active-low option.

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

### Electrical power measurement (energy)

Expose a power reading to Home Assistant via the Matter **Electrical Power
Measurement** cluster (`ActivePower`, plus optional `Voltage`, `ActiveCurrent`
and `Frequency`). Use an **Electrical Power Measurement (server)** slot.

Two forms of `endpoint.set`:

- `endpoint.set("power", watts)` — sets **ActivePower** only (watts). Leaves
  voltage/current/frequency unchanged.
- `endpoint.set("power", { power = W, voltage = V, current = A, frequency = Hz })`
  — sets any subset; omitted fields are left unchanged.

Units are the natural ones (W / V / A / Hz); the firmware scales them to the
Matter mW/mV/mA/mHz representation internally.

| Setting | Value |
|---|---|
| Endpoint Type | Electrical Power Measurement (server) |
| Trigger | Periodic |
| Period | 2000 (2 seconds) |

Example — report power derived from the analog input (0–100 % → 0–3680 W, i.e.
a 16 A / 230 V circuit) plus a fixed mains voltage:

```lua
local max_w = 3680          -- 16 A @ 230 V
local mains_v = 230

function run()
  local duty = input.analog()               -- 0..100 %
  local watts = duty / 100 * max_w
  endpoint.set("power", {
    power   = watts,
    voltage = mains_v,
    current = watts / mains_v,
  })
  log("power=" .. math.floor(watts) .. " W")
end
```

Minimal variant (ActivePower only):

```lua
function run()
  endpoint.set("power", input.analog() / 100 * 3680)
end
```

> Note: on the **1PM Gen4 (BL0942)** and **2PM Gen4 (ADE7953)** the built-in
> metering already creates its own Electrical Power Measurement endpoint(s)
> straight from the hardware — you do **not** need a Lua slot for those. This
> slot type is for exposing a power value you compute or read yourself (e.g.
> from an external sensor via the analog input) on models without onboard
> metering.

---

## 8. Multi-input script (different behavior per button)

Use `input.button_id()` to distinguish which physical input triggered the event.
This is also how a **TTP223 touch pad on the Add-on Digital IN** (`id == 1`)
becomes a second light switch: it gets the same short/long/double-press events
as the wall switch, so a touch surface can toggle or dim a bound light.

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

## 11. Mode cycle on the SW input: on → off → LDR control

A wall switch on **SW** steps through three modes; every change of the switch
position (either direction) advances to the next mode:

| Mode | Relay (e.g. fluorescent tube) | Bound lamp |
|---|---|---|
| ON | on | on |
| OFF | off | off |
| LDR | automatic, from the analog IN light sensor | automatic level, off when bright |

| Setting | Value |
|---|---|
| Endpoint Type | OnOff Toggle + Dim + Color (client) |
| Trigger | Periodic |
| Period | 500 (poll SW every 0.5 s; the LDR control runs every 10th run = 5 s) |

A script slot has one trigger, so the switch is polled with `input.sw()` from
the periodic run instead of using a button-event trigger. The first run only
records the current switch position; the script starts in LDR mode. Only SW is
watched — the Add-on Digital IN, the PCB button and SW2 do not affect the mode.

```lua
-- Trigger: Periodic, 500 ms.
-- Elke standwissel van SW schakelt de regeling door:
--   0 = AAN   (TL-relais + lamp aan)
--   1 = UIT   (TL-relais + lamp uit)
--   2 = LDR   (automatische lichtregeling, elke 5 s)
-- en daarna weer 0 = AAN enzovoort.

local LDR_PERIOD_RUNS = 10       -- 10 x 500 ms = 5 s

local tl_lower_level_off   = 55
local tl_lower_level_on    = 60
local bulb_upper_level_on  = 65
local bulb_upper_level_off = 70
local tl_upper_level_on    = 100
local tl_upper_level_off   = 101

local last_level   = -1
local tl_state     = false
local bulb_state   = false
local sun_override = false

local mode    = 2                -- start in LDR-regeling
local last_sw = nil
local runs    = 0

local function ldr_reset()
  last_level   = -1
  sun_override = false
end

local function set_all(on)
  output.relay_set(on)
  tl_state = on
  if on then
    endpoint.command("on")
  else
    endpoint.command("off")
  end
  bulb_state = on
  last_level = -1
end

local function ldr_run()
  local duty = 100 - input.analog()
  log("duty=" .. duty .. "%")

  if not sun_override and duty >= tl_upper_level_off then
    sun_override = true
  elseif sun_override and duty <= tl_upper_level_on then
    sun_override = false
  end

  if sun_override then
    if tl_state then
      output.relay_set(false)
      tl_state = false
      log("duty=" .. duty .. "% -> TL OFF (zon)")
    end
    if bulb_state then
      endpoint.command("off")
      bulb_state = false
      last_level = -1
      log("duty=" .. duty .. "% -> bulb OFF (zon)")
    end
    return
  end

  if (not tl_state) and duty >= tl_lower_level_on then
    output.relay_set(true)
    tl_state = true
    log("duty=" .. duty .. "% -> TL ON")
  elseif tl_state and duty <= tl_lower_level_off then
    output.relay_set(false)
    tl_state = false
    log("duty=" .. duty .. "% -> TL OFF")
  end

  if bulb_state and duty >= bulb_upper_level_off then
    endpoint.command("off")
    bulb_state = false
    last_level = -1
    log("duty=" .. duty .. "% -> bulb OFF")
  elseif (not bulb_state) and duty <= bulb_upper_level_on then
    bulb_state = true
  end

  if bulb_state then
    endpoint.command("on")
    -- map 0..bulb_upper_level_off -> level 1..254
    local level = math.floor(duty * 253 / bulb_upper_level_off) + 1
    if level > 254 then level = 254 end
    if level ~= last_level then
      endpoint.command("move_to_level", {level=level, transition=10})
      last_level = level
      log("duty=" .. duty .. "% -> bulb level " .. level)
    end
  end
end

function run()
  local sw = input.sw()
  if last_sw == nil then
    last_sw = sw                 -- eerste run: alleen stand onthouden
  elseif sw ~= last_sw then
    last_sw = sw
    mode = (mode + 1) % 3
    if mode == 0 then
      set_all(true)
      log("SW -> mode AAN")
    elseif mode == 1 then
      set_all(false)
      log("SW -> mode UIT")
    else
      ldr_reset()
      runs = LDR_PERIOD_RUNS     -- direct regelen
      log("SW -> mode LDR")
    end
  end

  if mode == 2 then
    runs = runs + 1
    if runs >= LDR_PERIOD_RUNS then
      runs = 0
      ldr_run()
    end
  end
end
```

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
- `endpoint.command("move_with_onoff", {up=bool, rate=N})` — `up=true` sends an
  explicit On first, so it works on a lamp that is off; `up=false` on a lamp that
  is already off is accepted by the lamp but changes nothing
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
- `endpoint.set("power", watts)` — Electrical Power Measurement, ActivePower only (W)
- `endpoint.set("power", { power=, voltage=, current=, frequency= })` — any subset (W / V / A / Hz)

### Utilities
- `log(msg)` — print to serial log
- `timer.millis()` → integer (uptime in ms)
