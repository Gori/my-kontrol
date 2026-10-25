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
- BACK, ENTER and the encoder touch get new numbers from 44 up.

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
  - A 16-segment font goes in `ui/font16.c`.

A screen the model can't express is a bug in the model, not a reason to draw in a board.

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

## Tests

- **Host tests (`firmware/test`):** the core and drivers, against fake buses.
- **`tools/xcore/xsim.py`:** the same tests as XS1 code, so the compiler output is checked too.
- **`sim/`:** the full core on a Mac, with the S49 shown as text: 9 displays and the light guide.
- **Hardware:** every item in the board's hardware doc is confirmed on a keyboard before release.
