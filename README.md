# Oskar's Firmware for the Komplete Kontrol M32

Replacement application firmware for the Native Instruments Komplete Kontrol M32.
It runs standalone: no Komplete Kontrol, Native Access or other NI software.
NI's bootloader stays untouched, so the stock firmware can always be restored.

## Status

| Area | State |
|---|---|
| Flashing, recovery, cold boot | Done, tested on hardware |
| Display (128x32 SSD1306), fonts, menus | Done |
| USB MIDI, two ports: "M32 Keys" and "M32 DAW" | Done |
| Smart Play: scale (quantize / easy), chords, arpeggiator, MIDI clock sync | Done, host tests + hardware MIDI tests |
| Settings saved in flash, survive firmware updates | Done |
| Mackie Control (Logic, Ableton) protocol | Done over USB; not yet tested in a DAW |
| Keybed with velocity, 21 buttons, 4D encoder, 8 knobs + touch, strips | Done, tested on hardware |
| Button LEDs | Driver answers; visual check pending |
| Pedal input (2 footswitches, CC 64/65) | Done; tested unplugged only |
| Knob pages (4 × 8 CCs), 8 user presets | Done, presets tested on hardware |

## Layout

```
firmware/   application firmware (C, TinyUSB)
  test/     host tests (Smart Play) and UI preview renderer
tools/      m32dfu.py (flash), m32diag.py (diagnostics), m32test.py (inject UI/key events),
            mksplash.py, bdf2c.py, ghidra/ (decompiler export script)
fw/         NI stock images extracted from NI's updater (recovery: m32_v140.bin)
re/         reverse-engineering output (Ghidra project, decompilation, hardware map)
fonts/      Spleen bitmap fonts (BSD-2-Clause)
```

## Build

Requires `arm-none-eabi-gcc` (Homebrew) and Python with `pyusb` (libusb from Homebrew).

```
cd firmware
make                 # build/m32.bin
```

Host tests:

```
cd firmware/test
cc -I.. test_smartplay.c ../smartplay.c -o /tmp/t && /tmp/t
cc -I.. ui_preview.c ../ui.c ../gfx.c ../settings.c ../smartplay.c -o /tmp/p && /tmp/p /tmp
```

## Flash

```
cd tools
python3 m32dfu.py enter                        # app -> NI bootloader (DFU)
python3 m32dfu.py flash ../firmware/build/m32.bin
```

`enter` works from both NI's firmware and this firmware.

## Recovery

The NI bootloader (0x08000000–0x08002FFF) is never written. This firmware falls back to it on its own:

- any CPU fault -> bootloader
- a hang -> 2 s hardware watchdog -> next boot goes to the bootloader
- clock failure at boot -> bootloader

If the M32 shows "Firmware update in progress", it is in the bootloader; flash again.
To go back to stock: `python3 m32dfu.py flash ../fw/m32_v140.bin`.

## How the NI bootloader decides what to run

Flag page at 0x0801FC00 (first 8 bytes):

| Bytes | Meaning |
|---|---|
| `55 5A A5 AA 59 95 3A A3` | stay in DFU (written by the app before resetting) |
| `59 95 3A A3 55 5A A5 AA` | application valid (written by the bootloader after a download) |
| anything else | no valid application: stay in DFU |

The bootloader does not checksum the image. It takes its DFU-mode USB strings
from the application area.

## Controls

Transport and DAW buttons always send Mackie Control on the DAW port. The knobs
and the 4D encoder follow **System > DAW Mode** (MIDI or Mackie).

| Button | Press | Shift + press |
|---|---|---|
| Scale | scale mode on/off | open Scale & Chord menu |
| Arp | arpeggiator on/off | open Arpeggiator menu |
| Tempo | tap tempo (internal clock) | |
| Octave < / > | octave −/+ | Octave <: fixed velocity on/off |
| Browser | open / close the menu | |
| Undo | MCU Undo | Redo (MCU Shift+Undo) |
| Quantize / Ideas | MCU F1 / F2 (assign in the DAW) | MCU F5 / F6 |
| Loop / Metro | MCU Cycle / Click | |
| Play / Rec / Stop | MCU Play / Record / Stop | Play: Rewind |
| Preset up / down | MIDI mode: knob page −/+; Mackie mode: MCU Up / Down (track select) | Mackie: MCU Bank left / right |
| M / S | MCU Mute / Solo, strip 1 | |
| Plug-in / Track | MCU Plug-in / Pan assignment | MCU Instrument / Send |

| Control | MIDI mode | Mackie mode |
|---|---|---|
| Knobs 1–8 | CC 14–21 (Knobs menu), value popup | V-pots 1–8; touch = fader touch |
| 4D turn | opens the menu; in the menu moves/edits | jog wheel |
| 4D push / up / down / left / right | menu navigation; left/right on the home screen = transpose −/+ | MCU Enter / Up / Down / Left / Right |
| Pitch strip | 14-bit pitch bend, centre on release | same |
| Mod strip | CC 1 (Knobs menu), holds its value | same |

Lights: Shift, Scale, Arp, Octave show their state; Tempo flashes on the beat;
Loop, Metro, Play, Rec, Stop, M, S, Plug-in, Track mirror the DAW.

## MIDI ports

| Port | Carries |
|---|---|
| M32 Keys | notes, Smart Play output, knob CCs; MIDI clock in for the arpeggiator |
| M32 DAW | Mackie Control (select "Mackie Control" in Logic / Ableton, input and output = M32 DAW) |

## Flash map

| Address | Use |
|---|---|
| 0x08000000–0x08002FFF | NI bootloader (never touched) |
| 0x08003000–0x0801B7FF | application |
| 0x0801B800 | NI knob names (kept for restoring stock) |
| 0x0801E000 | 8 user presets |
| 0x0801E800, 0x0801F000 | settings log (two pages) |
| 0x0801F800 | NI settings, calibration, serial (never written) |
| 0x0801FC00 | bootloader flag page |
