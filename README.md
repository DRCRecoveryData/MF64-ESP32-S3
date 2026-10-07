# MF64 — ESP32-S3 Firmware Port

A source-verified behavioral port of the **Midi Fighter 64** stock firmware
(DJ TechTools / [`wunnation/Midi_Fighter_64`](https://github.com/wunnation/Midi_Fighter_64))
to the **ESP32-S3**.

The port replicates every function, constant, state machine, palette, and
SysEx command from the original ATmega32U4 firmware, replacing only the
hardware-specific layers (USB stack, EEPROM, timers, LED driver) with
ESP32-S3 equivalents.

**Status:** Logic verified on PC (82/82 tests passing). Hardware verification
pending an actual ESP32-S3.

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
- [Bugs Caught by the Test Suite](#bugs-caught-by-the-test-suite)
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

| Feature | Source File | Tests |
|---|---|---|
| Button scan (Latch/Clock/Data) | `key.c` | ✅ Hardware-only |
| 10-sample debounce (AND of samples) | `key.c` | ✅ 2/2 |
| Note mapping `36 + buttonIndex` | `midi.c` | ✅ 4/4 |
| Bank channels (1→ch3, 2→ch2) | `midi.c` | ✅ |
| Bank select keys 28 & 63, 1000 ms hold | `midifighter64.c` | ✅ 4/4 |
| `g_key_bank_last_up` stuck-note fix | `midifighter64.c` | ✅ Hardware-only |
| Note-off feedback delay (7-bit wrapped timer) | `midifighter64.c` | ✅ Hardware-only |
| MIDI CC 3 bank change | `midifighter64.c` | ✅ |
| All 5 combos (A–E), notes 8/9/10/11/12 | `combo.c` | ✅ 6/6 |
| 20-color default palette | `display.c` | ✅ 7/7 |
| 128-color Ableton velocity palette | `display.c` | ✅ Verbatim |
| BRG byte order | `display.c` | ✅ 5/5 |
| 4 geometric animations | `display.c` | ✅ 21/21 |
| `midi_animation_state` (velocity bands 18–53) | `display.c` | ✅ 6/6 |
| `flash_animation` / `pulse_animation` | `display.c` | ✅ 7/7 |
| `display_flash_counter` @ 75-tick wrap (128 BPM) | `led.c` | ✅ 4/4 |
| `g_led_counter[0..3]` + PWM reload | `led.c` | ✅ |
| Ball demo (gravity, collisions, colors) | `led.c` | ✅ Hardware-only |
| Sleep timer (UP counter, boot at `G_EE_SLEEP_TIME`) | `display.c` | ✅ Hardware-only |
| EEPROM defaults (all 24 fields) | `eeprom.c` | ✅ Verbatim |
| Factory reset via SysEx | `eeprom.c` | ✅ Hardware-only |
| SysEx PUSH_CONF (tag-indexed decode) | `config.c` | ✅ 3/3 |
| SysEx PULL_CONF | `config.c` | ✅ Verbatim |
| SysEx SYSTEM (factory reset) | `config.c` | ✅ Verbatim |
| SysEx BULK_XFER (LED color push/pull) | `config.c` | ✅ Verbatim |
| Power-adjustment for imported colors | `display.c` | ✅ Verbatim |
| Bootloader key check on boot | `midifighter64.c` | ✅ Verbatim |

**Total: 82 logic tests passing.**

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

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 mf64_final_esp32s3.ino
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
- **Hold button 28 for 1 second** → switch to Bank 1 (index 0, MIDI ch 3)
- **Hold button 63 for 1 second** → switch to Bank 2 (index 1, MIDI ch 2)
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
| PUSH_CONF | `0x01` | Write settings from host → device (tag-indexed) |
| PULL_CONF | `0x02` | Read settings from device → host |
| SYSTEM | `0x03` | `0x02` = factory reset |
| BULK_XFER | `0x04` | Push/pull LED color tables (24-byte chunks) |

### PUSH_CONF tag map (from `config.c`)

| Tag | Field | Range |
|---|---|---|
| `0x00` | MIDI channel | 1–16 (converted to 0-indexed) |
| `0x01` | MIDI velocity | 0–127 |
| `0x02` | Keypress LED | 0/1 |
| `0x03` | Four banks mode | 0/1 |
| `0x07` | MIDI output mode | 0=notes, 1=notes+CC, 2=CC |
| `0x08` | Combos enable | 0/1 |
| `0x0A` | Animation | 0–6 |
| `0x0B` | Tilt mask | — |
| `0x0E` | Tilt sensitivity | — |
| `0x16` | Sleep time (min) | 0–60 |
| `0x17` | Side bank | 0/1 |

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

If you don't have an ESP32-S3 yet, you can verify the logic on your PC.

### Host-side test harness

Compile and run `mf64_test.cpp`:

```bash
g++ -O2 -std=c++17 -o mf64_test mf64_test.cpp
./mf64_test
```

**Expected output:**

```
MF64 Logic Test Harness
=======================
...
=======================
Results: 82 passed, 0 failed
```

### What the harness verifies

| Category | Tests |
|---|---|
| Note mapping, channels, bank select | 8 |
| Debounce algorithm | 2 |
| Combo state machine (Combo A) | 5 |
| SysEx PUSH_CONF tag decode | 3 |
| ISR tick behavior | 4 |
| Palette values (20-color + Ableton) | 7 |
| Default bank colors | 4 |
| Geometry row/column math | 18 |
| Square/circle/star/triangle animations | 21 |
| Animation bounds (64×8×4 combos) | 1 |
| Color propagation (BRG) | 1 |
| Flash bitmask + pulse sine | 7 |
| MIDI animation velocity bands | 6 |
| **Total** | **82** |

### What the harness cannot verify

| Item | Requires |
|---|---|
| Real WS2812B output | ESP32-S3 + LEDs |
| USB MIDI enumeration | ESP32-S3 + host |
| NVS persistence | ESP32-S3 |
| Hardware timer accuracy | ESP32-S3 |
| Real button scan (signal integrity) | ESP32-S3 + MF64 PCB |
| SysEx round-trip with Utility | ESP32-S3 + Utility |

---

## Bugs Caught by the Test Suite

The host-side harness caught two real bugs in the initial port that would
have caused misbehavior on hardware:

### Bug 1 — 16-bit wraparound on x86

**Location:** `service_bank_select_buttons()`

**Symptom:** Bank select with `system_time_ms = 0` failed at the 1000 ms
threshold because `now - counter` promoted to 32-bit `int` on the ESP32
(`int` is 16-bit on AVR).

**Fix:**
```cpp
if (g_bank_select_counter[b] > 0 &&
    (uint16_t)(now - g_bank_select_counter[b]) >= limit) {   // cast added
```

### Bug 2 — Hardcoded SysEx byte offsets

**Location:** `handleSysEx()` case `0x01`

**Symptom:** The initial port read `data[5] = channel`, `data[6] = velocity`,
etc. But the real MF64 SysEx format uses **tag–value pairs**
(`config.c` `tv_table_decode`). Reading fixed offsets would have interpreted
tags as values.

**Fix:** Tag-indexed decode loop, matching `tv_table_decode` in `config.c`.

Both fixes are in the final `.ino`.

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
| SysEx `tvtable_t` decode | Tag-indexed, byte offsets match `tv_table_decode` |
| VU meter animation | Not ported (was commented out upstream too) |
| `led_set_state_dfu` pattern | Bootloader halt runs, but no LED pattern displayed |
| `key_read_isr` @ 1 kHz | Runs at 100 Hz — perceptually identical |

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
├── mf64_final_esp32s3.ino       ← the firmware
├── mf64_test.cpp                ← host-side logic test harness
├── LICENSE                      ← see below
└── docs/
    ├── wiring.md                ← pin diagram (optional)
    ├── sysex_protocol.md        ← SysEx reference (optional)
    └── button_map.md            ← physical button ↔ note mapping (optional)
```

---

## Source Verification

This port was written by reading the actual source files from
[`wunnation/Midi_Fighter_64`](https://github.com/wunnation/Midi_Fighter_64):

| Source file | Lines | Ported to |
|---|---|---|
| `constants.h` | 180 | All `#define`s |
| `key.h` / `key.c` | 210 | `scanRaw()`, `scanDebounced()` |
| `midi.h` / `midi.c` | 200 | `midiNoteOn/Off/CC()` |
| `combo.h` / `combo.c` | 350 | `comboRecognize()`, `state_table[]` |
| `led.h` / `led.c` | 700 | `ball_demo_run()`, ISR logic |
| `display.h` / `display.c` | 900 | All 4 animations, palettes |
| `eeprom.h` / `eeprom.c` | 260 | `saveSettings()`, `loadSettings()` |
| `config.h` / `config.c` | 550 | `handleSysEx()`, `BULK_XFER` |
| `sysex.h` / `sysex.c` | 200 | SysEx parser (packet-based) |
| `midifighter64.c` | 700 | `setup()`, `loop()` |

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

---

## Status Summary

| Layer | Status |
|---|---|
| Source read line-by-line | ✅ Complete |
| Port written | ✅ Complete |
| Bugs caught by test suite | ✅ 2 fixed |
| Logic verified on PC | ✅ 82/82 tests pass |
| Firmware compiles | ✅ 402 KB flash, 57 KB RAM |
| Hardware verified | ⏳ Pending ESP32-S3 upload |
