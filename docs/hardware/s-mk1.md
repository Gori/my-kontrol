# Komplete Kontrol S-Series MK1 (S25, S49, S61, S88)

Sources:
- **M**: NI, *Komplete Kontrol 1.9.1 manual, hardware MK1* (page).
- **CE**: NI, *Controller Editor manual*, 2017 (page).
- **KB**: NI support article "List of Compatible Pedals with Komplete Kontrol MK1 / MK2 Keyboards".
- **S**: NI's MK1 controller scripts, installed with Maschine 2 (`Scripts/Maschine/KompleteKontrolS`). They give NI's names for the controls and lights.
- **F**: stock firmware 1.2.0 (XMOS image and SAMD image from NI's updater), static analysis.
- **E**: NI's firmware run in our emulator (`tools/xcore/s49emu.py`), watching the modelled hardware. Tentative: the emulator isn't the silicon.
- **T**: tested on an S49.

No tag: not confirmed.

Read in full:
- **M**: all 198 pages.
- **CE**: chapters 1–4 and 24–26. Chapters 5–23 cover other controllers and don't mention the S-series.
- **KB**: the article above.

Not found anywhere in NI's documents:
- dimensions;
- release notes for the MK1 firmware.

## Models

| | S25 | S49 | S61 | S88 |
|---|---|---|---|---|
| Keys | 25 | 49 | 61 | 88 |
| Keybed (M 46) | Fatar semi-weighted, aftertouch | same | same | Fatar hammer action |
| USB PID, app (F) | 0x1340 | 0x1350 (T) | 0x1360 | 0x1410 |
| USB PID, update mode | | 0x1352 (T) | | |
| S49 key notes (F) | | 36–84 | | |

- **Same panel:** every model has the same panel and features; only the keybed differs (M 46).
- **S88 revisions:** the S88 had at least 3 hardware revisions (CE 301):
  - Revision 2 can swap the expression pedal's tip and ring.
  - Revision 3 adds a continuous (half-damper) sustain input.
- **Firmware images:** the XMOS image is per model. The SAMD image is identical for the S25, S49 and S61, and differs for the S88 (F).

## Panel (M 17, 46–53; CE 255–266)

- **PERFORM:** SHIFT, SCALE (EDIT), ARP (EDIT).
- **TRANSPORT:**
  - LOOP, RWD, FFW.
  - PLAY (RESTART), REC (COUNT-IN), STOP.
- **TRANSPOSE:** OCT−, OCT+. Pressing both resets the octave.
- **Touch strips:** pitch (left) and mod (right). Each has an LED chain in blue and white (CE 265).
- **Control section:**
  - 2 page buttons (◀ ▶).
  - 8 endless knobs. They are touch-sensitive.
    - NI treats them as absolute "analog encoders" (CE 343). The 9 values from sensor MCU 0x12 fit that (F).
  - 9 displays, all with the same layout (T), top to bottom:
    - a meter that fills from left to right;
    - a text line;
    - a second text line.
  - Placement and use:
    - Display 1 sits left of the knobs, with PAGE and PRESET printed beside it. In NI's MIDI mode its two text lines are the page and the preset or template name (CE 264).
    - Displays 2–9 sit under knobs 1–8.
      - In NI's MIDI mode they show the assignment name.
      - Touching the knob shows its value for a moment (CE 264).
      - The meter fills from the left or from the middle (CE 264).
    - Each text line is 8 characters, 16-segment (T: both lines, using NI's font).
- **NAVIGATE:**
  - BROWSE, INSTANCE, BACK.
  - The Control encoder, which turns and pushes (M 58, 61). The manual also calls it the BROWSE encoder (M 62).
    - It is also touch-sensitive (S: `BUTTON_NAVIGATE_ENCODER_CAP`). The manuals don't mention this.
- **23 buttons** in total (M, S): SHIFT, SCALE, ARP, the 6 transport buttons, OCT− and OCT+, the 2 page buttons, BROWSE, INSTANCE, BACK, ENTER, PRESET ▲ ▼ and the 4 arrows.
  - On top of those: the encoder push, the encoder touch and 8 knob touches.
  - PRESET ▲ ▼.
  - ◀ ▲ ▼ ▶ and ENTER.
- **Light Guide:** one RGB LED above each key (T).
- **Button lights:**
  - The hardware dims each light in 128 steps (F).
  - NI's MIDI mode uses only two levels, dim and bright, with the brightness of each set in preferences (CE 266, 301).
  - Colours: PLAY is green and REC is red (M 48 drawing). The others look white.
  - Lights exist for all 23 buttons:
    - NI's MK1 script uses lights for SHIFT, SCALE, ARP, REC, both page buttons, BROWSE, INSTANCE, BACK, ENTER, PRESET ▲ ▼ and the arrows (S).
    - The transport and OCT lights are in CE 266.
    - NI's report 0x80 has 25 values for them (F). What the 2 extra values drive is unknown.
- **No buttons above the displays.**
  - The manual mentions "Button 1–8" on M 11, 91 and 99. That text is left over from the MK2.
  - The MK1 drawings on M 46 and M 50 show no such buttons.

## Rear (M 32–33; CE 256, 301)

Left to right:
- Kensington slot.
- MIDI IN, MIDI OUT (5-pin DIN).
- PEDALS:
  - SUSTAIN: a stereo jack, with tip and ring read as two switches. Its polarity is detected at power-up.
  - EXPRESSION: continuous. It can be calibrated and its range inverted.
- USB-B.
- Power input and power switch.

Pedals (KB):
- NI lists TS sustain switches, with and without a polarity switch.
- It also lists TRS two-way pedals, but doesn't say which jack they go in. SUSTAIN reads tip and ring separately (CE 256), so it fits.
- It lists TRS expression pedals.
- NI lists two TRS half-damper pedals, but doesn't say which MK1 input or revision supports them. CE 301 says continuous sustain needs an S88 revision 3.

Power:
- The supply comes in the box (M 12). The rear panel says **15 V DC, 1.2 A** (T: label on an S49).
- Without USB, the keyboard runs from the supply and sends on 5-pin MIDI, on channel 1 by default (M 86, CE 268).
- NI's standalone mode needs firmware 52 or later (CE 267). NI's firmware keeps one template on the keyboard ("Set as Standalone Default", CE 269).

## Electronics (F unless tagged)

| Part | What | Link |
|---|---|---|
| Main CPU | XMOS XS1, 2 tiles × 64 KB RAM (XS1-L16A-128 class), external ULPI USB PHY | — |
| Keybed scanner | separate MCU | UART to tile 0 port 1K |
| Sensor MCU 0x15 | ATSAMD20G17 or G18, 48 MHz internal clock | I²C, SWD target 1 |
| Sensor MCU 0x12 | unknown, likely a SAMD | I²C, SWD target 0 |
| Button inputs | 4 × 8-bit parallel-in shift registers | tile 0 |
| Lights | 3 serial-in shift-register chains, 74HC595 type | tile 0 |
| Displays | 3 ST7565-family controllers, each driving 3 displays | tile 0, bit-banged SPI |
| Flash | SPI, 1 MB: AT25DF081A or MX25L8006E | tile 1 |
| 5-pin MIDI | software UART | tile 1 |

Port names are per tile: 1J on tile 0 is not 1J on tile 1. The reference clock is 100 MHz (10 ns ticks).

### Keybed (tile 0)

- **Link:**
  - RX on 1K, 31 250 Bd, 8N1, LSB first.
  - 4E bit 0 is driven high at start.
- **Message:** 3 bytes.
  - `0x90 | v_hi` or `0x80 | v_hi`, then the note, then `v_lo`.
  - Velocity is 10 bits: `(status & 7) << 7 | v_lo`.
- **No aftertouch on this link.** The manual says the S25, S49 and S61 send channel pressure (M 46, CE 257), so it arrives some other way. The likely source is the 9th value of MCU 0x12 (unconfirmed).

### Buttons and encoder (tile 0)

- **Pins:** clock 1B (1.163 MHz), load 1C (active low), data 1A.
- **Reading:** 32 bits, scanned every 1 ms. Inputs are active low. A 3-sample debounce is applied.
- **Encoder:** byte 1, bits 0–1 are the Control encoder's quadrature.
- **Model ID:** byte 0, bits 7–5, read once at power-up. Reads 7 in the emulator (all released); the real S49 value is unknown.
- **Power-up combo:** byte 3 == 0xCF changes the start-up path. Purpose unknown.
- **Bit map (E, tentative):** in the emulator, toggling single input bits with NI's firmware in standalone mode: bits 5 and 6 send CC64 and CC65; bits 28 and 29 switch the knob page (CC14–21 ⇄ CC22–29). The other 28 bits had no visible effect in that mode. Whether bits 5/6 are the pedal switches or panel buttons isn't settled; it conflicts with the guess that the pedals are on sensor 0x15 (see below).

### Lights (tile 0)

| Chain | Clock | Data | Latch | Bits | Drives |
|---|---|---|---|---|---|
| A | 1D (1.5625 MHz) | 1G | 1E | 72 | light guide, first part |
| B | 1D | 1J | 1L | 80 (75 used) | light guide, rest |
| C | 1I (1.282 MHz) | 1M | 1N | 112 (109 used) | buttons, touch-strip LEDs |

- **Shifting:** last byte first, each byte LSB first, then a latch pulse.
- **Outputs:** active low.
- **Brightness:** software PWM, 7 bits, linear, 10 ms period. The three chains are offset by 0, 2.5 and 5 ms.
- **Light guide:** 3 chain bits per key (R, G, B). Bit order is permuted within each 24-bit group.
- **Touch-strip LEDs:** likely 2 strips × 21 positions × 2 colours (blue, white) = 84, which matches NI's HID report 0x81.

### Displays (tile 0)

- **4A:**
  - Bits 0–2 are the chip selects of controllers 0–2, active low.
  - Bit 3 is A0 (0 = command, 1 = data).
- **4B:** bit 0 RESET (active low), bit 1 SDA, bit 2 SCK. Bit 3 is set after init; purpose unknown.
- **Signalling:** MSB first, about 1 MHz. Data changes while SCK is low.
- **Reset:** RESET low for 100 µs, then wait 1 ms.
- **Init:** `A3 A0 C0 24 81 03 2C`, wait 1 ms, `2E`, wait 1 ms, `2F`, wait 1 ms, `AF A4`.
- **Refresh, per controller:**
  - `40 00 B0 12 04`, then 120 bytes.
  - `40 00 BF 12 04`, then 120 bytes.
- **Layout:**
  - Each character is 16 segments, stored as 5 bytes of display RAM.
  - Each controller holds 24 characters per text line: 3 displays × 8.
  - Cell layout, decoded from NI's font and renderer code:
    - Bits 0–1 are the top segments, left and right.
    - Bits 2–3 are the right side, upper and lower.
    - Bits 4–5 are the bottom segments, right and left.
    - Bits 6–7 are the left side, lower and upper.
    - Bits 11–12 are the middle bar, left and right.
    - Bits 9 and 14 are the centre vertical, upper and lower.
    - Bits 8, 10, 13 and 15 are the diagonals: top-left, top-right, bottom-left and bottom-right.
    - There is no dot inside a cell.
  - Row 0 holds the meter and the dots for each display, in cells 0–7 (T, all bits tested):

    | Bits | Lights |
    |---|---|
    | cells 0–7, bit 0 | meter blocks 1–8, left to right |
    | cells 0–7, bit 1 | a thin line at meter positions 1–8 |
    | cell 7, bits 8 and 9 | meter position 9: a block and a thin line |
    | cell 0, bit 2 | a box around the meter |
    | cells 0–6, bit 8 | the dot after characters 1–7 of line 1 |
    | cells 0–6, bit 9 | the dot after characters 1–7 of line 2 |

  - There is no dot after the 8th character of either line.
  - The other row-0 bits do nothing (F).
- **Backlight:** PWM on 1O, about 1 kHz, 0–100 %.

### Sensor MCUs (tile 0)

- **Bus:**
  - I²C: SCL 1H, SDA 1F, bit-banged, ≤ 400 kHz, with clock stretching.
  - Polled every 10 ms: write `03 05`, then read.
- **0x15 reply:** `[05, len, ADC u16, 48 × s16 touch, switch bits, 0]`. The length is 102, or 104 on the S88.
  - The touch values are levels minus baseline. Detection happens on the XMOS.
  - The ADC is likely the expression pedal. The 2 switch inputs are likely the sustain tip and ring.
  - Its other commands are in the firmware notes.
- **0x12 reply:**
  - 22 bytes: header, 9 × u16, 2 bytes.
  - Likely the 8 knob positions plus aftertouch.
  - Its firmware isn't in NI's updaters.
- **Start-up writes:** `60` + 11 bytes to 0x15 (ignored by its firmware 1.2.0), and `60` + 26 bytes to 0x12.
- **SWD to the sensor MCUs:**
  - SWCLK on 4C bit 0, SWDIO on 1P.
  - nRESET on 4F: bit 2 for target 0, bit 1 for target 1.
  - NI's updater uses this to reflash 0x15. My Kontrol never does.

### 5-pin MIDI (tile 1)

- **Ports:** OUT 1J, IN 1L.
- **Timing:** 31 250 Bd, 8N1. RX samples 1.5 bits after the start edge.
- **Routing:** the stock firmware routes MIDI IN to USB and USB to MIDI OUT. Its UART has no thru.

### Flash (tile 1)

- **Ports:** MISO 1C, SS 1A, CLK 1B, MOSI 1D.
- **Layout (F; flash byte addresses in a 1 MB chip):**
  - 0x0: boot loader (word 0 = its size in words, then the loader, then a CRC).
  - 0x0C: the boot-partition size word (read at run time; 0 = whole flash). Not known statically.
  - `4·n + 8`: the factory image (NI fw 0x14, confirmed present by the revert test).
  - Next sector after it: the upgrade image, at most 0x20000. With the factory image ~0x17200, the upgrade area ends below ~0x40000.
  - NI's own data (outside libflash, each with an A and B copy, erased in 4 KB sectors):
    - 0x4C000 / 0x4D000: identity/calibration (word 0 = 0x13500100 + a magic).
    - 0x4E000 / 0x4F000: NI's settings.
    - 0x50000 / 0x54000: the MIDI template store (2780 B).
  - Everything from the boot-partition size to the top is libflash's data partition, which NI never uses.
- **Our firmware:**
  - Must never write 0x0–0x54FFF, so restoring NI's firmware gives back an untouched keyboard (like the M32 rule).
  - Keeps its settings at **0xF0000** (in the unused region; confirmed clear of NI's areas).
- **Write protection:** NI leaves the chip's status-register protection bits set after a write. Our flash driver must clear them before writing and may set them again after.

## Stock USB

- **Speed:** high speed.
- **Interfaces:**
  - 0: audio control (UAC2, no streaming).
  - 1: MIDI, 2 ports: keyboard, and "EXT" for the 5-pin jacks.
  - 2: HID (report descriptor 1555 bytes).
  - 3: DFU runtime (DFU 1.1, transfer size 64, attributes 0x07).
- **bcdDevice:** the firmware version, 0x0045 for 1.2.0 (T).
- **HID output reports (F, T):**
  - The keyboard accepts light and display reports only after report `A0 00 02`.
  - `0x80` (25 B) and `0x81` (84 B): button and strip LEDs, 0–127.
  - `0x82` (147 B): light guide, 49 × RGB, 0–127.
  - `0xE0` (248 B): display cells, as `x, y, w, h` (u16) followed by u16 segment masks. The grid is 72 × 3: 9 displays × 8 characters, 3 rows.
  - `0xA1`–`0xAF`: MIDI-mode template data.
  - `0xF4` with sub-command `0x30` may save the template to flash. Don't send it.

## Update mode (F, T)

1. **Enter:** `21 F2 0000 0003` (XMOS_DFU_RESETINTODFU).
   - The device reboots as PID `+2` and sets a RAM flag (0x11042011).
   - No button combination into update mode was found in the firmware.
2. **Write:** DFU_DNLOAD in 64-byte blocks with the whole image file. DFU_UPLOAD reads the installed image back (T).
3. **Leave:** `21 F3 0000 0000`.

Image format:
- The file is stored bit-reversed per byte.
- **Header:** magic 0x01A551E5, CRC, length, and 2 segments, one per tile.
- **Segments:** each is loaded at 0x10000 and ends with a CRC.
- **CRC:** the XS1 `crc32` step, polynomial 0xEDB88320.
- **Header word 4:** differs per model.
  - The boot loader checks it (T). With NI's value, an image with changed segment contents boots. With 0, 0x7FFFFFFF or 0xFFFFFFFF, the loader falls back to the factory image.
  - My Kontrol packages its images with XMOS's `xflash`.

**Recovery:**
- The flash holds a factory image: NI firmware 0x14.
- **Falling back to it was tested on an S49 (T):**
  1. Enter update mode.
  2. Send `21 F1 0000 0000` (XMOS_DFU_REVERTFACTORY). This deletes the upgrade image.
  3. Leave update mode. The keyboard boots the factory image (bcdDevice 0x0014), with the same interfaces and update mode as 1.2.0.
  4. Writing 1.2.0 from there restores it; the read-back is identical.
- **Failed checks:** if an upgrade image fails its checks, the loader boots the factory image (F).
- **Hang:** an image that passes the checks but hangs before USB starts can't receive the revert command. Our firmware therefore needs its own way back to the factory image: a power-up button combination, and falling back after repeated failed starts.
- **DFU_UPLOAD** reads back only the upgrade image (F, T).
- **XMOS_DFU_SELECTIMAGE** (`F4`) does nothing (F).

## Open

- Bit map of the 32 button inputs.
- Each of the 16 character segments, lit one at a time. Full glyphs read correctly (T); single segments weren't tested.
- What the 0x12 chip is, and its 9th value.
- The touch-point map of 0x15: 8 knobs, the encoder and the 2 strips.
- What the 2 extra light values in report 0x80 drive.
- Aftertouch source.
- Pedal wiring, and how sustain polarity is detected.
- The boot-partition-size word at flash 0x0C (only readable at run time), and the power-up combo (byte 3 == 0xCF).
- The other models: key ranges, the S88 keybed, and the S88 revisions.
