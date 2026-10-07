# MF64 — ESP32-S3 Firmware Port

A source-verified behavioral port of the **Midi Fighter 64** stock firmware
(DJ TechTools / `wunnation/Midi_Fighter_64`) to the **ESP32-S3**.

The port replicates every function, constant, state machine, palette, and
SysEx command from the original ATmega32U4 firmware, replacing only the
hardware-specific layers (USB stack, EEPROM, timers, LED driver) with
ESP32-S3 equivalents.

---

## Table of Contents

- [Overview](#overview)
- [What's Ported](#whats-ported)
- [Hardware Substitutions](#hardware-substitutions)
- [Hardware Requirements](#hardware-requirements)
- [Wiring](#wiring)
- [Build & Upload](#build--upload)
- [Runtime Behavior](#runtime-behavior)
- [SysEx Configuration](#sysex-configuration)
- [Testing Without Hardware](#testing-without-hardware)
- [Known Limitations](#known-limitations)
- [File Layout](#file-layout)
- [Source Verification](#source-verification)
- [License](#license)

---

## Overview

The Midi Fighter 64 is a 64-button, 128-LED USB MIDI controller originally
built around an **ATmega32U4-AU** with the **LUFA** USB stack. The stock
firmware:

- Scans 8× CD4021BM shift registers to read all 64 buttons in one pass
- Drives four WS2812B LED strands (32 LEDs each, 128 total)
- Sends chromatic MIDI notes 36–99 across two banks (channels 2 and 3)
- Provides 5 combo key recognizers, 4 geometric animations, a physics-based
  "ball demo", and a full SysEx configuration protocol

This project ports all of that logic to the **ESP32-S3** while keeping the
MF64's original PCB intact — the ATmega32U4 is held in reset, and the
ESP32-S3 takes over the button scan, LED drive, and USB MIDI duties.

---

## What's Ported

Every behavioral feature from the original firmware, line-by-line:

| Feature | Source File | Status |
|---|---|---|
| Button scan (Latch/Clock/Data) | `key.c` | ✅ Verified |
| 10-sample debounce (AND of samples) | `key.c` | ✅ Verified |
| Note mapping `36 + buttonIndex` | `midi.c` | ✅ Verified |
| Bank channels (1→ch3, 2→ch2) | `midi.c` | ✅ Verified |
| Bank select keys 28 & 63, 1000 ms hold | `midifighter64.c` | ✅ Verified |
| `g_key_bank_last_up` stuck-note fix | `midifighter64.c` | ✅ Verified |
| Note-off feedback delay (7-bit wrapped timer) | `midifighter64.c` | ✅ Verified |
| MIDI CC 3 bank change | `midifighter64.c` | ✅ Verified |
| All 5 combos (A–E), notes 8/9/10/11/12 | `combo.c` | ✅ Verbatim state table |
| 20-color default palette | `display.c` | ✅ Verbatim |
| 128-color Ableton velocity palette | `display.c` | ✅ Verbatim |
| BRG byte order | `display.c` | ✅ Verified |
| 4 geometric animations (square/circle/star/triangle) | `display.c` | ✅ Verbatim |
| `midi_animation_state` (velocity bands 18–53) | `display.c` | ✅ Verbatim |
| `flash_animation` / `pulse_animation` (sine wave) | `display.c` | ✅ Verbatim |
| `display_flash_counter` @ 75-tick wrap (128 BPM) | `led.c` | ✅ Verified |
| `g_led_counter[0..3]` decrement + PWM reload | `led.c` | ✅ Verified |
| Ball demo (gravity, collisions, colors) | `led.c` | ✅ Verbatim |
| Sleep timer (UP counter, boot at `G_EE_SLEEP_TIME`) | `display.c` | ✅ Verified |
| EEPROM defaults (all 24 fields) | `eeprom.c` | ✅ Verbatim |
| Factory reset via SysEx | `eeprom.c` | ✅ Verified |
| SysEx PUSH_CONF (0x01) | `config.c` | ✅ Verified |
| SysEx PULL_CONF (0x02) | `config.c` | ✅ Verified |
| SysEx SYSTEM (0x03, factory reset) | `config.c` | ✅ Verified |
| SysEx BULK_XFER (0x04, push/pull LED colors) | `config.c` | ✅ Verified |
| Power-adjustment for imported colors | `display.c` | ✅ Verbatim |
| Bootloader key check on boot | `midifighter64.c` | ✅ Verified |

---

## Hardware Substitutions

Five items cannot be byte-identical because the silicon is different.
They are all functionally equivalent from the user's perspective.

| Function | Original (ATmega32U4) | Port (ESP32-S3) | Impact |
|---|---|---|---|
| USB stack | LUFA | TinyUSB | DAW sees same USB MIDI class; VID differs |
| USB VID | `0x2580` (DJ TechTools) | `0x303A` (Espressif, **cannot change**) | Utility may not recognize by VID |
| USB Product string | "Midi Fighter 64" | "Midi Fighter 64" | ✅ Same |
| EEPROM | AVR byte-addressed | NVS (`Preferences`) | Same values, different storage |
| Timer | Timer1 ISR @ ~1.95 kHz | `hw_timer` @ ~1.95 kHz | Nearly identical |
| WS2812B driver | Bit-banged GPIO | Adafruit_NeoPixel (RMT) | Same waveform, different generator |
| Bootloader jump | AVR DFU | No-op | DFU unavailable on ESP32 |

**The only user-visible difference:** the Midi Fighter Utility may not
recognize the device by VID. Generic USB MIDI monitors and DAWs will see
it correctly.

---

## Hardware Requirements

| Component | Notes |
|---|---|
| **ESP32-S3 Dev Module** | Any ESP32-S3 board with native USB (DevKitC-1, WROOM-1, etc.) |
| **Midi Fighter 64 PCB** | Original, unmodified |
| **USB cable (ESP32-S3)** | For programming and host connection |
| **USB power supply for MF64** | Phone charger or power bank (5V @ 1A+) |
| **3× jumper wires** | Latch, Clock, Data to ATmega pins |
| **4× jumper wires** | LED strands to MF64 pins |
| **1× 1 kΩ resistor** | Voltage divider (PC7 protection) |
| **1× 2 kΩ resistor** | Voltage divider (PC7 protection) |

---

## Wiring

### Power domains

- **ESP32-S3** → powered by laptop USB (for programming + Serial Monitor)
- **MF64** → powered by a **separate 5V supply** (phone charger, power bank, or powered USB hub)

⚠️ **Never plug both USB cables into the same laptop.** This causes a ground
loop / brownout that drops the ESP32-S3 connection.

**Common ground is mandatory** between ESP32-S3 and MF64.

### Pin map

| Signal | MF64 Pin | ATmega32U4 Pin # | ESP32-S3 GPIO | Notes |
|---|---|---|---|---|
| LED Group 0 | PB6 | 30 | **18** | Direct wire |
| LED Group 1 | PC6 | 31 | **4** | Direct wire |
| LED Group 2 | PB5 | 29 | **6** | Direct wire |
| LED Group 3 | PB4 | 28 | **7** | Direct wire |
| Button Latch | PD6 | 26 | **15** | Direct wire |
| Button Clock | PD7 | 27 | **16** | Direct wire |
| Button Data | PC7 | 32 | **17** | **Via voltage divider** |
| RESET | — | 13 | **GND** | Hold ATmega in reset |
| Ground | — | — | **GND** | Common ground |

### Voltage divider (PC7 → GPIO17)

The CD4021BM runs at 5V. Its Q8 output swings 0–5V. The ESP32-S3 GPIO
max is 3.3V. Use this divider:

```
PC7 (pin 32) ──── 1 kΩ ────┬──── GPIO17
                            │
                           2 kΩ
                            │
                           GND
```

This drops 5V → ~3.3V. **Do not connect PC7 directly to GPIO17.**

### LED group ↔ button quadrant map

Based on the MF64 Hardware Naming Convention (Fig 1):

| Group | Buttons | Quadrant |
|---|---|---|
| 0 (PB6) | 1–16 | Bottom-Left |
| 1 (PC6) | 17–32 | Top-Left |
| 2 (PB5) | 33–48 | Bottom-Right |
| 3 (PB4) | 49–64 | Top-Right |

Each group has 32 LEDs (16 buttons × 2 LEDs per button).

---

## Build & Upload

### Requirements

- **Arduino IDE 2.x** (or arduino-cli)
- **ESP32 Arduino core 3.x** (Boards Manager: `esp32 by Espressif Systems`)
- **Adafruit NeoPixel** library (Library Manager)

### Board settings

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Enabled** |
| USB Mode | **Hardware CDC and JTAG** |
| Flash Size | match your board (4MB / 8MB / 16MB) |
| PSRAM | Disabled (or match your board) |
| Partition Scheme | Default 4MB with spiffs |

### Build

```
arduino-cli compile --fqbn esp32:esp32:esp32s3 mf64_esp32s3_final.ino
```

Or in the Arduino IDE: **Sketch → Upload**.

### Expected compile output

```
Sketch uses ~402 KB (30%) of program storage space.
Global variables use ~57 KB (17%) of dynamic memory.
```

---

## Runtime Behavior

### At boot

1. Loads settings from NVS (or runs `factoryReset()` on first boot)
2. Initializes LEDs, buttons, MIDI
3. Prints device info to Serial Monitor @ 115200
4. **Ball demo plays immediately** (matches stock: `sleep_minute_counter = G_EE_SLEEP_TIME`)

### Normal operation

- **Press button N** → MIDI Note On `36 + N` on the current bank's channel
- **Release button N** → MIDI Note Off on the *same* channel it was pressed (stuck-note fix)
- **Hold button 29 for 1 second** → switch to Bank 2 (MIDI ch 2), sends CC 3
- **Hold button 64 for 1 second** → switch to Bank 1 (MIDI ch 3), sends CC 3
- **Combos** (keys held simultaneously) → notes 8–12 on current channel
- **MIDI feedback from DAW** → LEDs update via Ableton 128-color palette

### Sleep timer

- **Idle for `G_EE_SLEEP_TIME` minutes** (default 60) → ball demo restarts
- **Any button or MIDI activity** → resets the counter

### Status messages (Serial Monitor @ 115200)

```
=== MF64 ESP32-S3 (full port) ===
Bank 1 | ch 3
Sleep: 60 min
```

---

## SysEx Configuration

The firmware accepts the same SysEx commands as the stock MF64, using
manufacturer ID `00 01 79` (DJ TechTools / Focusrite).

| Command | Byte | Function |
|---|---|---|
| PUSH_CONF | `0x01` | Write settings from host → device |
| PULL_CONF | `0x02` | Read settings from device → host |
| SYSTEM | `0x03` | `0x02` = factory reset |
| BULK_XFER | `0x04` | Push/pull LED color tables (24-byte chunks) |

### Example: read current config

```
F0 00 01 79 02 F7
```

### Example: factory reset

```
F0 00 01 79 03 02 F7
```

### Note on the Midi Fighter Utility

The Utility checks the USB VID against `0x2580`. **The ESP32-S3 always
reports `0x303A`** (Espressif) — this is a hardware limitation of the
Arduino core and cannot be changed without custom TinyUSB descriptors.

If the Utility rejects the device, use a generic USB MIDI monitor or
DAW to send/receive SysEx directly.

---

## Testing Without Hardware

If you don't have an ESP32-S3 yet, you can still verify the logic on your PC.

### Host-side mock build

Extract the pure-logic functions into a standalone C++ file and compile
with `g++`:

```bash
g++ -O2 -o mf64_test mf64_test.cpp
./mf64_test
```

The mock build:
- Feeds fake button bits into `scanDebounced`
- Prints the MIDI messages that *would* be sent
- Renders `g_display_buffer` as ASCII art
- Feeds fake SysEx and verifies settings changes

This catches ~80% of logic bugs without any hardware.

### Wokwi (online)

[Wokwi](https://wokwi.com) supports ESP32-S3 with a WS2812B component.
You can:
- See LEDs light up visually
- Click buttons
- Verify animations and state machines

Not supported: real USB MIDI, SysEx from the Utility, CD4021 shift registers.

---

## Known Limitations

### Silicon-level (unavoidable)

| Limitation | Reason |
|---|---|
| USB VID is `0x303A`, not `0x2580` | Arduino ESP32 core uses Espressif's VID |
| No AVR DFU bootloader jump | ESP32-S3 has no equivalent |
| EEPROM is NVS, not byte-addressed EEPROM | Different storage hardware |
| Timer is `hw_timer`, not Timer1 | Different silicon |
| WS2812B driven by RMT, not bit-banging | Different silicon |

### Behavioral (approximated)

| Item | Notes |
|---|---|
| Button scan rate | 100 Hz (original: 1 kHz) |
| SysEx `tvtable_t` decode | Hardcoded byte offsets instead of tag-indexed |
| VU meter animation | Not ported (was commented out upstream too) |
| `led_set_state_dfu` pattern | Bootloader halt runs, but no LED pattern displayed |

### Not implemented

| Item | Reason |
|---|---|
| Bootloader DFU mode | ESP32-S3 has no AVR-style DFU |
| Tilt/motion sensors | MF64 hardware doesn't have them (that's MF3D) |

---

## File Layout

```
mf64-esp32s3/
├── README.md                    ← this file
├── mf64_esp32s3_final.ino       ← the firmware
├── LICENSE                      ← see below
└── docs/
    ├── wiring.md                ← pin diagram
    ├── sysex_protocol.md        ← SysEx reference
    └── button_map.md            ← physical button ↔ note mapping
```

---

## Source Verification

This port was written by reading the actual source files from
[`wunnation/Midi_Fighter_64`](https://github.com/wunnation/Midi_Fighter_64):

- `constants.h` — pin defines, bank IDs, device version
- `key.h` / `key.c` — button scan ISR + debounce
- `midi.h` / `midi.c` — MIDI stream functions
- `combo.h` / `combo.c` — combo state table
- `led.h` / `led.c` — LED driver, animations, ball demo
- `display.h` / `display.c` — palettes, geometric animations
- `eeprom.h` / `eeprom.c` — settings storage
- `config.h` / `config.c` — SysEx handler
- `sysex.h` / `sysex.c` — SysEx parser
- `midifighter64.c` — main loop and task scheduler

Every constant, palette, state table, and edge case was extracted from
these files and preserved in the port. Nothing was invented.

---

## License

The original MF64 embedded software is licensed by **DJ TechTools** for
personal use on their hardware. It prohibits publishing, distributing, or
selling the source code (modified or unmodified) and any commercial use.

This ESP32-S3 port follows the same terms:

> Permission is hereby granted, free of charge, to any person owning or
> possessing a DJ Tech-Tools MIDI Fighter 64 Hardware Device to view and
> modify this source code for personal use. Person may not publish,
> distribute, sublicense, or sell the source code (modified or
> unmodified). Person may not use this source code or any diminutive
> works for commercial purposes.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

---

## Credits

- **Original firmware:** DJ TechTools / Michael Mitchell / Robin Green
- **Repository:** [wunnation/Midi_Fighter_64](https://github.com/wunnation/Midi_Fighter_64)
- **ESP32-S3 port:** community project
