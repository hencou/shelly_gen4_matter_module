# Shelly 1L Gen4 / 2L Gen4: stock analysis (STES protocol)

Status: **analysis only, nothing implemented.** Everything below comes from
static disassembly of the stock images `S1LG4` 2.0.1 and `S2LG4` 2.0.1. None of
it has been checked on a device. Items marked *open* are not established yet.

## Architecture

The ESP32-C6 does not drive the relays. A separate STM32 microcontroller
switches bistable (latching) relays and is controlled over a UART with a small
framed protocol that stock calls **STES** (`shelly_stes.cpp`,
`shelly_stes_latch_relay.cpp`). The ESP can also flash the STM through the
STM32 ROM bootloader (`shelly_stm32bl*.cpp`, BOOT and RESET lines driven by the
ESP). Stock does that at boot from `lpsu.bin` when the STES version does not
match ("STES protocol version different!").

## Pins (identical on 1L and 2L)

| Function | GPIO | Evidence |
|---|---|---|
| SW1 | 6 | `InputPeripheral(id 0, gpio 6, 1, 2)` |
| SW2 | 3 | `InputPeripheral(id 1, gpio 3, 1, 2)` |
| Onboard button | 7 | `InputPeripheral(id 999, gpio 7, 0, 1)`, as on the other models |
| Status LED | 9 | output `999, gpio 9` passed to the `sys_led` constructor |
| Relay(s) | — | via the STM, not a GPIO |
| NTC | *open* | `shelly_temp_sensor_ntc.cpp` is present; pin not established |
| STM UART number / RX / TX / baud | *open* | come from a runtime config struct (log: `STM pin configuration: Uart %d, rx %d, tx %d, boot %d, reset %d`) |
| STM BOOT0 / RESET | *open* | same |

The switch inputs use flag value `2` where the 1 Gen4 wall switch uses `0`;
what that flag selects is *open*.

## Frame format

Both directions:

```
AA 55 | LEN | CMD | PAYLOAD (LEN-1 bytes) | CHK
CHK = ~((LEN - 1) + CMD + sum(PAYLOAD)) & 0xFF
```

- The `AA 55` preamble is a 2-byte constant in `.data`. The receiver syncs on
  `AA`, `55`, then reads `LEN`.
- The receiver collects `LEN` bytes (CMD + payload) and compares the trailing
  `CHK` with the same formula ("STES protocol checksum error!").
- The sender limits the payload to 8 bytes.
- Replies from the STM carry `CMD | 0x80`.

## Commands (ESP → STM)

| CMD | Payload | Meaning |
|---|---|---|
| `0x30` | `[0x00]` | Handshake / version. Until it is answered, every other reply is rejected ("STES not established …"). |
| `0x50` | `[relay]` | Relay ON (`relay` = 0 or 1, `0x51 - on`) |
| `0x51` | `[relay]` | Relay OFF |
| `0x52` | — | Read relay state; sent after the handshake, at most twice ("Just read Relay %d") |
| `0x63` / `0x64` | `0x64`: 16-bit big-endian value | Config get/set ("ParseConfig …") |

## Replies (STM → ESP)

- `0xB0` (handshake): `payload[0] = 0` (status), `payload[1] = 0`,
  `payload[2] = 1` → established ("STES protocol version equal [%d,%d]").
  Bytes 5–12, when present, are stored as version/config info.
- `0xE3` / `0xE4`: config replies.
- Any other reply: `payload[3..4]` is a 16-bit value. The frame is then handed
  to the registered handlers; the latch-relay handler (`ParseLRelay`) checks
  the relay count (`STES_NUM_LRELAYS`) and a per-relay state. The exact
  status layout is *open*.

## Timing

- 1000 ms timer at init.
- 400 ms reply timeout after each send ("STES protocol no STM reply!").
- 50 ms between handling a reply and the next send.
- Whether the STM expects a periodic keepalive is *open*.

## What is needed before implementing

1. The UART number, pins and baud of the STM link. These need either deeper
   tracing of the config struct, or a serial capture on a device.
2. The relay-status reply layout, and the 2L relay index mapping.
3. A unit to test on, powered from an isolated supply. Without a neutral the
   PCB sits at mains potential.
4. A safe fallback: never send the STM bootloader sequence, and do not switch
   when the handshake fails.
