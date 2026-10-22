# Adding a keyboard

Every Komplete Kontrol keyboard is a microcontroller with a keybed, some
buttons, lights and a screen, behind NI's USB bootloader. My Kontrol keeps
everything that does not depend on the hardware in `firmware/core`.
Supporting another model means writing one board folder and, for a new chip
family, one platform folder.

```
firmware/
  core/                 portable: never touches hardware, builds on the host
    hal.h               THE interface a board and a platform implement
    controls.h          logical button IDs (CTL_*), shared by all boards
  boards/<name>/        one per keyboard
    board.mk            PLATFORM, LDSCRIPT, BOARD_SRCS
    board_config.h      key count, knob count, display size, USB IDs, flash map
    board.c             board_info and the board_* functions from hal.h
  platform/<family>/    one per MCU family: startup, clocks, flash, watchdog, bootloader handshake
    platform.mk         compiler and flags
```

`make BOARD=<name>` builds it.

## What a board provides

See `core/hal.h`. In short:

| Function | Job |
|---|---|
| `board_early_init`, `board_boot_combo` | display up; report whether the recovery buttons are held at power-up |
| `board_init` | everything else |
| `board_tick` | every 100 µs from the platform tick: scan the keybed and matrix, queue events |
| `board_poll` | hand the main loop one `mk_event_t` at a time |
| `board_light` | button lights, by `CTL_*` |
| `board_display_page` | write one 8-pixel band of the framebuffer |
| `board_usb_pullup` | the D+ pull-up, if the board switches it |
| `board_serial`, `board_diag` | the USB serial number, and extra bytes for `tools/mkdiag.py` |

Each event is one of:

| Event | Value |
|---|---|
| key down / up | velocity, 0 = released |
| button | as `CTL_*`, 1 pressed / 0 released |
| encoder | detents |
| knob | steps, + = clockwise |
| knob touch | 1 touched / 0 released |
| strip | 0..16383, or −1 when released |
| pedal | 1 pressed / 0 released |

The board does the hardware maths: dead zones, debouncing and velocity
timing. The core never sees a pin.

## Things that differ between models

- **Controls:**
  - Map your matrix positions to `CTL_*`.
  - Missing controls: append IDs from 32 up in `controls.h`. Never renumber them, because the host protocol uses the numbers.
- **Keybed:** `BOARD_KEYS` and `board_info.base_note`.
  - S-series keybeds have aftertouch. Add an event type for it; the core can grow.
- **Display:**
  - The UI is laid out for 128×32 monochrome (M32, A-series).
  - The S-series colour screens need a new renderer behind the same UI model in `core/ui.c`. Open an issue before starting, so we can plan it together.
- **Lights:** `board_light` gets a logical control and a level 0..255. RGB key lights (Light Guide) would be a new `board_key_light` call.
- **USB:**
  - Keep NI's VID/PID in `board_config.h`, so NI's bootloader, NI's updater and the web installer still find the keyboard.
  - The bootloader's DFU PID goes in the web installer's board table (`web/js/images.js`).
- **Flash map:**
  - Never write NI's bootloader, its flag page, or NI's settings and calibration. Then restoring NI's firmware gives back an untouched keyboard.
  - Find free pages and put them in `board_config.h`.

## How the M32 was worked out

1. **Get NI's firmware image.** It sits unencrypted inside NI's updater app. `web/js/stock.js` finds it by its vector table signature.
2. **Find the MCU.** The vector table gives the stack top and the reset address. The register addresses the code touches give the chip family.
3. **Decompile.** Use Ghidra with `tools/ghidra/DumpDecomp.java`. Then follow the GPIO, SPI, I²C and ADC setup to the pins, and the USB descriptors to the protocol.
4. **Learn the bootloader handshake.**
   - The flag page and magic values are written just before NI's firmware resets into DFU.
   - Check that the bootloader validates nothing more than that, otherwise flashing your build will fail.
5. **Bring the board up a piece at a time.** Use a developer build (`make DEV=1`) and these tools:
   - `tools/mkpeek.py` reads memory.
   - `tools/mktest.py` injects events.
   - `tools/mkdiag.py` shows which sensors answer.
6. **Write it all down** in `docs/hardware/<name>.md`, like [the M32 notes](hardware/m32.md).

**Never** commit NI's firmware, decompiled code or updaters. Facts about
the hardware belong in the docs; NI's code does not.

## Safety while porting

The NI bootloader is your safety net, so never write it. The platform layer
has three more:

- a fault or a hang reboots, and three in a row stop in the bootloader
- the boot combo forces the bootloader at power-up
- every update goes through the stock DFU path

Get these working first on a new platform. The M32 was brought up without
a debugger this way.

## Checklist for a pull request

- [ ] `make BOARD=<name>` builds with no warnings, and `make test` passes.
- [ ] `docs/hardware/<name>.md` describes pins, matrix, lights and the flash map.
- [ ] `board_info.id` gets the next free number, listed in `docs/protocol.md`.
- [ ] The web installer's board table has the new USB IDs and the stock image signature.
- [ ] Tested on hardware: every key, button, light, knob and strip, plus recovery with the boot combo.
