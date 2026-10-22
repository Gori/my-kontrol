# My Kontrol

*Open-source firmware for Native Instruments Komplete Kontrol keyboards.*

## Your keyboard finally works on its own without the need for any software, plugins or drivers.

Plug it into any computer, phone or synth that speaks USB MIDI and play.
Scales, chords, arpeggiator, note repeat and strum all run on the keyboard.
Logic and Ableton Live can drive the screen and the lights.

**Status: beta.** Runs on the Komplete Kontrol M32. More keyboards are welcome, see [Adding a keyboard](docs/porting.md).

## Install

1. Open the **[web installer](https://my-kontrol.vercel.app)** in Chrome or Edge.
2. Plug in the keyboard and click **Install**.

That's it. To go back to NI's firmware, the same page restores it from NI's official updater.

Something went wrong? Hold **Shift + Browser** while plugging in. The keyboard
starts in update mode and the installer can flash it again. NI's bootloader is
never touched, so the keyboard can always be recovered.

## What it does

- **Smart Play**
  - 18 scales, in quantize or easy mode (white keys only).
  - Chords, plus a chord you teach it: hold **Ideas** and play a chord.
  - Strum.
- **Arpeggiator**
  - Modes: up, down, up/down, order, random, chord.
  - 1/4 to 1/32 including triplets, gate, swing, hold.
  - Runs on internal tempo or follows MIDI clock.
- **Note repeat** on **Quantize**: tap to toggle, hold for momentary. Velocity can follow the mod strip.
- **Keys**
  - Velocity curves and fixed velocity.
  - Split the keyboard into two zones on two channels.
  - Octave and transpose.
- **Controls**
  - 8 knobs × 4 pages of CCs.
  - 14-bit pitch strip, mod strip, two footswitches.
- **Ableton Live**: a control-surface script in [`daw/ableton`](daw/ableton) gives three views on the keyboard:
  - device parameters with names and values
  - mixer
  - a clip launcher grid
- **Logic, Live and others**: Mackie Control on the "My Kontrol DAW" port. Transport, track names and V-pots work.
- **8 presets**, and settings that survive updates.

The [manual](docs/manual.md) explains every button.

## Build it yourself

```sh
git clone --recursive https://github.com/Gori/my-kontrol
cd my-kontrol/firmware
make            # needs arm-none-eabi-gcc
make test       # host tests, no keyboard needed
```

Flash from the command line with `python3 tools/mkflash.py enter` and then
`python3 tools/mkflash.py flash firmware/build/m32/firmware.bin` (needs `pyusb`).

## Project layout

| Path | What |
|---|---|
| `firmware/core` | everything that is the same on every keyboard |
| `firmware/boards` | one folder per keyboard: pins, keybed, lights, display |
| `firmware/platform` | one folder per chip family |
| `daw/ableton` | Ableton Live script |
| `web` | the web installer |
| `docs` | [manual](docs/manual.md), [porting guide](docs/porting.md), [host protocol](docs/protocol.md), [M32 hardware notes](docs/hardware/m32.md) |

## Thanks and legal

GPL-3.0. Uses [TinyUSB](https://github.com/hathach/tinyusb) (MIT), CMSIS
(Apache-2.0), the [Spleen](https://github.com/fcambus/spleen) fonts (BSD-2) and
[fflate](https://github.com/101arrowz/fflate) (MIT). See [NOTICE](NOTICE.md).

My Kontrol is an independent project. It is not affiliated with or endorsed by
Native Instruments. Komplete Kontrol is a trademark of Native Instruments. No
NI code or firmware is included. Installing other firmware is at your own
risk, though the original firmware can always be restored.
