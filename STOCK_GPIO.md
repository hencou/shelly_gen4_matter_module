# GPIO mapping

The per-model pin table in `main/hw_config.c` used to come from published Gen4
pinout pages and an ESPHome device config. This document records where each pin
actually comes from: the official Shelly stock firmware images.

## Method

Stock images are fetched per model from the official lookup endpoint that this
firmware already uses for return-to-stock:

```
https://updates.shelly.cloud/update/<app code>
```

| Model | App code | Version analysed |
|---|---|---|
| Shelly 1 Gen4 | `S1G4` | 2.0.0 |
| Shelly Mini 1 Gen4 | `Mini1G4` | 2.0.0 |
| Shelly 1PM Gen4 | `S1PMG4` | 2.0.0 |
| Shelly Mini 1PM Gen4 | `Mini1PMG4` | 2.0.0 |
| Shelly 2PM Gen4 | `S2PMG4` | 2.0.0 |


## Result

| Function | 1 Gen4 | Mini 1 Gen4 | 1PM Gen4 | 2PM Gen4 |
|---|---|---|---|---|
| Relay | 5 | 10 | 4 | 5 + 3 |
| Wall switch | 10 | 12 | 10 | 11 + 10 |
| Onboard button | 4 | 22 | 1 | 12 |
| Status LED | 15 | 5 | 11 | 18 |
| Add-on Analog IN | 17 | — | 17 | 17 |
| Add-on Digital IN | 18 | — | 12 | 1 |
| Add-on 1-Wire in/out | 16 / 9 | — | 16 / 9 | 16 / 9 |
| Power meter | — | — | BL0942 UART0, 7 + 6 | ADE7953, IRQ 19 |


Notes on individual findings:

- **Add-on Digital IN is not the same pin on every model.** This firmware drove
  GPIO18 on all of them, which on a 2PM is the status LED. Digital IN is now a
  profile field (`addon_digital_gpio`) instead of a Kconfig constant.
- **Mini has no Add-on.** Both Mini images contain no OneWire/DHT code at all
  (no `shelly_dht22.cpp`, no add-on pin struct), which confirms `has_addon =
  false` for that profile.
- **2PM per-model pin table.** Stock keys a pin table on the model string; for
  `S4SW-002P16EU` it holds `11, 10, 5, 3, 12, 18, 4, 19` — switch 1/2, relay
  1/2, button, LED, the NTC temperature-sensor ADC pin
  (`shelly_temp_sensor_ntc.cpp`, GPIO4) and the ADE7953 IRQ.
- **ADE7953 I2C SDA/SCL could not be recovered.** Stock takes them from the
  device configuration stored in NVS rather than from a compile-time constant,
  so the image does not contain them. The values in `hw_config.c` (SDA 6, SCL 7)
  remain unverified; the IRQ (19) is confirmed.
- **BL0942 UART.** The 1PM sets up UART0 with GPIO7 and GPIO6, matching the pair
  this firmware uses. Which of the two is TX and which is RX is not established
  by the call site, so that assignment is still unverified.

## Model string → app code

The stock images embed their own model strings, which is where the mapping in
`main/stock_fw.c` comes from:

| Model string | App code |
|---|---|
| `S4SW-001X16EU` | `S1G4` |
| `S4SW-001P16EU` | `S1PMG4` |
| `S4SW-002P16EU` | `S2PMG4` |

The Mini images carry no model string, so a Mini falls back to the app code for
the device type selected on the management page (`Mini1G4`).
