# How the keyboards share code

Status: proposal, written for the S-Series MK1 port. Hardware facts: [hardware/](hardware/).

## Layers

| Layer | Knows about | Shared by |
|---|---|---|
| `core/` | music, menus, the host protocol | every keyboard |
| `ui/` | how to draw the core's screen model on one kind of display | keyboards with that display kind |
| `drivers/` | one kind of chip or protocol, no pins | any board that has that chip |
| `platform/<mcu>/` | one MCU family: startup, clocks, flash, update path, USB device controller | boards on that MCU |
| `boards/<family>/` | pins, wiring, which drivers, per-size variants | the sizes of one model line |
| `usb/` | descriptors and classes (MIDI, HID, DFU runtime) on TinyUSB | every keyboard |
| `sim/` | the core on a Mac, drawing whatever the board describes | testing every keyboard |

A driver takes its bus operations from the board: pin set, pin read, delay, and byte transfer. So the same driver runs on an STM32 or an XMOS.

## Families and variants

`make BOARD=s49` builds family `s-mk1`, variant `s49`.

```
boards/
  m32/                    one size
  s-mk1/                  S25, S49, S61, S88
    board.c ...           shared by all four
    variants/s49/         board_config.h (keys, base note, PID, LED counts), surface.json
```

A variant only holds what differs: key count, lowest note, USB PID, light guide length, S88 sensor reply length.

## What the board tells the core

`board_info.caps` grows with the S-series features. A core feature runs only when its cap is set:

| Cap | M32 | S MK1 | Core feature |
|---|---|---|---|
| `CAP_LIGHT_GUIDE` | – | ✓ | scale, chord, key and DAW key colours (`core/lightguide.c`) |
| `CAP_STRIP_LEDS` | – | ✓ | strip value as bar, dot or steps |
| `CAP_ENCODER_TOUCH` | – | ✓ | the Control encoder is touch-sensitive (`CTL_ENCODER_TOUCH`) |
| `CAP_AFTERTOUCH` | – | ✓ | channel pressure |
| `CAP_KNOB_TOUCH` | ✓ | ✓ | (exists) |
| `CAP_EXPRESSION` | – | ✓ | continuous pedal input |

5-pin MIDI is a compile-time switch (`BOARD_MIDI_DIN`), not a cap, because it changes the USB descriptors and queues, not just a runtime feature.

New events:
- `EV_AFTERTOUCH`, 0..127.
- `EV_EXPRESSION`, 0..16383.

Controls:
- Most S-series controls already have `CTL_*` numbers:
  - INSTANCE, PAGE_LEFT and PAGE_RIGHT;
  - BROWSER (BROWSE), PRESET_UP and PRESET_DOWN;
  - NAV_PUSH (the encoder push).
- BACK, ENTER and the encoder touch get new numbers from 44 up; Rewind and Fast forward
  are 47 and 48.

## Screens

`core/ui.c` today draws straight onto a 128×32 framebuffer. It is split in two:

- **`core/ui.c`** decides *what* is shown. Its screen model has these fields:
  - a title and a subtitle;
  - 8 knob slots, each with a name, a value text, a bar 0..1 and a bipolar flag;
  - a notification;
  - menus, lists and the message screen.
- **`ui/oled.c`** draws the model on 128×32 pixels: the M32, and later the A-series. This is today's drawing code, moved here.
- **`ui/seg9.c`** draws the model on the S MK1's 9 segment displays:
  - Display 1 shows the title and the page.
  - Displays 2–9 show one knob each: the name, the value, and the bar.
  - The 16-segment font is `drivers/font16.c` (the glyphs are ours).

A screen the model can't express is a bug in the model, not a reason to draw in a board.

Implemented (core/screen.h, core/render.h, ui/oled.c). Refinements to the sketch above:
- A renderer is one call, `ui_renderer.show(screen_t*)`; it clears, draws the base
  screen, then the lost banner, then the popup. The board's Makefile picks the one
  renderer source (`BOARD_UI_SRCS`, default `ui/oled.c`), so ui/seg9.c is added
  without touching core/ui.c.
- A knob slot's bar is stored as `bar`/`bar_max` (plus a style and an optional meter
  line), not a 0..1 float, so no rounding is forced on the renderer.
- gfx.c/gfx.h/font.h stay in core/ as the OLED renderer's tools (the sim and the
  board push its framebuffer); they are not moved under ui/.
- Segment boards: `BOARD_SEG_DISPLAYS` (board_config.h) picks the text path. ui/seg9.c
  fills 9 `seg_display_t` (2 lines of 8 characters, a dot per character, a bar 0..255)
  and calls `board_seg_show()`; the board passes them to `drivers/st7565_seg.c`, which
  maps characters (`drivers/font16.c`) to segment RAM and sends it. main.c and app.c skip
  the pixel framebuffer on such boards. How each screen is laid out on the 9 displays is
  described at the top of ui/seg9.c.

## Drivers needed for the S MK1

| Driver | Chip / protocol | Reusable by |
|---|---|---|
| `drivers/st7565_seg` | ST7565 driving the 16-segment glass | S MK1 |
| `drivers/shift595` | serial-in LED chains with software brightness | S MK1, any shift-register lights |
| `drivers/shift165` | parallel-in button chains, debounced | S MK1 |
| `drivers/quadrature` | encoder decoding (moved from the M32 board) | M32, S MK1 |
| `drivers/ni_sensor` | NI's I²C sensor MCUs (0x15 touch, 0x12 knobs) | S MK1 |
| `drivers/ni_keybed_uart` | the S-series keybed scanner link | S MK1 |
| `drivers/velocity` | velocity curves from key timing or a raw value (moved out of the M32 board) | all |

## Platforms

| Platform | MCU | Toolchain | USB | Update path |
|---|---|---|---|---|
| `stm32f1` | STM32F103 | arm-none-eabi-gcc | TinyUSB fsdev | NI DFU bootloader |
| `xs1` | XMOS XS1, 2 tiles | clang (xcore) + `tools/xcore` | our ULPI driver as a TinyUSB DCD, full speed (not written yet; the biggest risk) | XMOS DFU into an upgrade image |

On the XS1, the board splits work over two tiles:

| Tile | Runs | Why |
|---|---|---|
| 1 | USB, 5-pin UART, flash | NI's firmware puts these there, and the flash and USB pins are on tile 1 |
| 0 | the core and the panel drivers | NI's firmware puts these there |

Timing-critical loops run in their own hardware threads: USB, the UARTs, LED brightness and the display bus. Tiles talk over channels, through one small message interface in `platform/xs1`.

Implemented (platform/xs1, boards/s-mk1):
- The tile link (`platform/xs1/link.h`): fixed chanends, tile 1 speaks first, packets
  `[type, n, payload]` + END for USB-MIDI, 5-pin bytes, flash requests and (developer
  builds) injected events.
- Tile 0: core/main.c with the HAL in `hal.c` (reference timer, a 100 µs tick thread, an
  optional `hal_idle()` that waits for link input or the next millisecond) and
  `tile0_link.c` in place of usb/usb_device.c and TinyUSB. Settings flash: the core's store
  reads the SPI flash on tile 1 by copy (`BOARD_FLASH_READ`), erases and writes it by request.
- Tile 1 (`tile1.c`): the link, the 5-pin UART (TX and RX threads) and the SPI flash
  (`drivers/spi_flash.c`: AT25DF081A or MX25L8006E by JEDEC ID, the protection bits cleared
  for each change and set again, nothing below 0x55000 ever erased or written, except the upgrade
  image slot in update mode). USB: TinyUSB on tile 1 over our ULPI driver
  (`dcd_xs1_ulpi.c`, `usb_ulpi.c`) with the descriptors in `usb/usb_link.c`: MIDI with 3
  cables (Keys, DAW, MIDI = the 5-pin port), a HID keyboard for the 4D encoder, a DFU
  runtime interface; USB-MIDI and keyboard taps cross the tile link. The ULPI wire layer is
  unverified and expected to need a redesign at bring-up (`platform/xs1/usb.md`, Risks);
  everything above it runs in the emulator.
- Update mode (`usb/dfu_xmos.c`, `tile1_dfu.c`, `upgrade.c`): XMOS RESETINTODFU / DETACH
  (or the menu, or both page buttons at power-up) set a RAM word and reboot; tile 1 comes up
  as a DFU-only device (PID 0x1352) that needs nothing from tile 0. DFU_DNLOAD writes the
  upgrade image slot (after NI's factory image, below 0x4C000), every page read back, the
  header page last after the image CRC checks; UPLOAD reads it back; REVERTFACTORY removes
  it so NI's factory image boots. Both page buttons held 10 s at power-up do that without
  USB.
- System (`hal.c`, `sswitch.c`): a software watchdog (the tick thread and the main loop
  watch each other, 2 s), reboot through the switch's PLL register, a boot log in flash
  (0xF4000: 3 starts in a row that never ran 10 s stop in safe mode), update mode as a stop
  with a marker on the displays (DFU: TODO), both page buttons at power-up = update mode,
  a unique ID hashed from NI's identity sector.
- S MK1 panel (boards/s-mk1, drivers/): the keybed UART (`ni_keybed`), input registers and
  encoder (panel.c), the sensor MCUs over bit-banged I2C (`ni_sensor`), the LED chains with
  bit-angle modulation (`shift595`) and the backlight, on 6 threads of tile 0. The light guide
  colours come from `core/lightguide.c` (scale, root, held keys). Unknown wiring is in tables
  marked TODO in panel.c and lights.c: input bits other than pedals, encoder and page buttons
  (so the transport, encoder push, Back and Enter have their control IDs but no bits yet);
  button and strip light bits; touch nodes; knob resolution.
- Packaging (`tools/xcore/s49pack.py`): an .xe from the two tile images (`tools/xcore/xe.py`,
  `boards/s-mk1/s-mk1.xn`), then XMOS's xflash: the same path rebuilds NI's 1.2.0 images
  byte for byte (except the page padding).
- Hybrid bring-up image (`make BOARD=s49 HYBRID=1 DEV=1`, `platform/xs1/ni_link.c`): our
  tile 0 next to NI's tile 1, speaking NI's tile link, so NI's USB and update path stay;
  the probe (`boards/s-mk1/probe.c`, `tools/s49probe.py`) sends the raw panel state out of
  NI's MIDI port and walks the LED bits.
- `make BOARD=s49` builds both tile images (`build/s49/tile0.bin`, `tile1.bin`);
  `tools/xcore/s49run.py` runs them on the board emulator; `xtest.py standalone` plays a
  whole standalone session on it, `xtest.py usb` the USB device above the wire.

## Tests

- **Host tests (`firmware/test`):** the core and drivers, against fake buses.
- **`tools/xcore/xsim.py`:** the same tests as XS1 code, so the compiler output is checked too.
- **`sim/`:** the full core on a Mac, with the S49 shown as text: 9 displays and the light guide.
- **Hardware:** every item in the board's hardware doc is confirmed on a keyboard before release.
