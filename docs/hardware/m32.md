# Komplete Kontrol M32 — hardware map from stock app firmware v1.40

Source: `re/m32_v140_decomp.c` (Ghidra) + disassembly/literal pools of `fw/m32_v140.bin` (load 0x08003000, file offset = addr − 0x08003000).
Tags: **[P]** = proven by code (function + evidence given), **[I]** = inferred, needs confirmation.
Bit-band aliases decoded as `0x42000000 + (reg−0x40000000)*32 + bit*4`.

Corrections to the first-pass notes:
- TIM1–TIM4 are **not used**: no timer base address appears in any literal pool, all TIM IRQ vectors are empty stubs. [P]
- EXTI is used only for the USB-wakeup line: `vec_58` writes EXTI_PR = 0x40000 (line 18). [P]
- AFIO only does the SPI1 remap (`FUN_08005ef4(1)` in `FUN_08009528`) and SWJ JTAG-disable, keeping SWD (`FUN_08005e30`, 0x300200). [P]
- PC0–PC3 and PB10/PB11 carry three bit-banged I²C buses. The runtime CRL/CRH rewrites switch SDA/SCL between open-drain output (nibble 7) and floating input (nibble 4). [P]
- Some functions are missing from the decomp: 0x0800A764–0x0800AC74, the USB class/vendor request handlers. They are reachable only through RAM function tables (0x200000E0) and were read from disassembly here.

---

## Pin table

| Pin | Function | Evidence | Conf. |
|---|---|---|---|
| PA0 | Button input (group 3 bit0 → button idx 0x16), IPU, active low | `FUN_08009658` (0x000F IPU), `FUN_08009720` reads GPIOA_IDR&0xF | high (input) / low (identity: 4D-encoder direction) |
| PA1 | Button idx 0x15, same as above | same | same |
| PA2 | Button idx 0x17 | same | same |
| PA3 | Button idx 0x18 | same | same |
| PA4–PA7 | ADC1 ch4–7, knob wiper inputs behind a 4:1 analog mux | `FUN_08003884` (AIN, ranks 1–4), `FUN_080033a4` | high |
| PA8 | USB D+ pull-up, high = attach | `FUN_0800acb4` BSRR/BRR 0x100 | high |
| PA9 | Reset of the mod-strip touch chip (bus B), OD, active low | `FUN_080068f0` init low, `FUN_08007af0`/`FUN_08007a64` pulse 0x422101A4 | high |
| PA10 | Reset of the pitch-strip touch chip (bus A), OD, active low | `FUN_080064fc`, `FUN_08008f84`/`FUN_08008ef8` pulse 0x422101A8 | high |
| PA11/12 | USB DM/DP | USB peripheral in use | high |
| PA13/14 | SWD (JTAG disabled, SWD kept) | `FUN_08005e30` | high |
| PA15 | LED power / LED-driver enable, high = on | `FUN_08006244` sets 1; cleared on USB suspend (`FUN_08005030`) and on the version screen (`FUN_08005e10`); set again on resume (`FUN_08009e58`) | medium |
| PB0, PB1, PB2 | Matrix row address A0, A1, A2 | `FUN_08009720` switch(row&7) writes 0x42218180/184/188 | high |
| PB3 | SPI1 SCK → OLED | `FUN_08009528` (AF_PP, SPI1 remap) | high |
| PB4 | OLED D/C | `FUN_08008ae4` writes 0x42218190 | high |
| PB5 | SPI1 MOSI → OLED | `FUN_08009528` | high |
| PB6 | LED-driver I²C SCL (bit-bang) | `FUN_080062a8`/`FUN_08006304`/`FUN_08006344`, 0x42218198 | high |
| PB7 | LED-driver I²C SDA (bit-bang; input with pull for ACK) | 0x4221819C write, 0x4221811C read in `FUN_08006384` | high |
| PB8, PB9 | Knob analog-mux select | `FUN_080033a4` 0x422181A0/1A4, 4 states | high |
| PB10 | I²C bus C SCL → knob-touch chip | `FUN_08006d0c`/`FUN_08006de4`: clock 0x422181A8, stretch-wait on IDR 0x42218128 | high |
| PB11 | I²C bus C SDA | data in/out via 0x422181AC / IDR 0x4221812C | high |
| PB12 | Pedal-jack plug detect (button idx 0x1A; "active" = pin high) | group-3 bit5 is set when PB12==0, then inverted; idx 0x1A starts the pedal arming timer (`FUN_08009980`) | medium |
| PB13 | Button idx 0x19, IPU, active low | group-3 bit4 | high (input) / low (identity: 4D-encoder push) |
| PB14 | Matrix row bank: 1 = rows 0–7 (keybed), 0 = rows 8–15 | 0x422181B8 set at row 0, cleared at row 8 | high (function) / medium (decoder topology) |
| PB15 | OLED CS, active low, configured open-drain | `FUN_08009658` OUT_OD, cleared before every OLED transfer | high |
| PC0 | I²C bus A SDA → pitch-strip touch chip | `FUN_08006580`/`FUN_08006670`: data 0x42220180, IDR 0x42220100 | high |
| PC1 | I²C bus A SCL (clock stretching honoured) | 0x42220184 / IDR 0x42220104 | high |
| PC2 | I²C bus B SDA → mod-strip touch chip | `FUN_08006974`/`FUN_08006a68`: 0x42220188 / 0x42220108 | high |
| PC3 | I²C bus B SCL | 0x4222018C / 0x4222010C | high |
| PC4 | Pedal jack contact 1: ADC ch14, or driven high while PC5 is sampled | `FUN_080033a4` toggles CRL nibble 4/5 and ADC rank-5 channel 14/15 | high (function) / low (tip vs ring) |
| PC5 | Pedal jack contact 2: ADC ch15, or driven high | same | same |
| PC6, PC7 | 4D-encoder quadrature A/B, IPU | `FUN_08009720` debounces IDR&0xC0; `FUN_08009980` derives direction → `FUN_08003e44` 4-bit counter | high (quadrature) / medium (that it is the 4D encoder) |
| PC8–PC15 | Matrix column inputs, IPU, active low; driven high briefly after every read | `FUN_08009720`: read IDR>>8, then CRH=0x33333333 + ODR\|=0xFF00, then CRH=0x88888888 | high |
| PD2 | OLED RESET | `FUN_08009528`, `FUN_08007f6c` | high |

Unused or not referenced: PD0/PD1 (HSE). No other pins are touched.

Clocks [P] (`FUN_08009204`, `FUN_0800acf8`): HSE×9 = 72 MHz, APB1 /2, APB2 /1, ADC /6 (12 MHz), USB /1.5. IWDG /32, reload 1250 (≈1 s), kicked in the main loop (`FUN_08006fec`).

**Time base**: SysTick LOAD = HCLK/40000 = 1800 (`FUN_0800a38e`). CLKSOURCE is never set, so the clock is HCLK/8 → **200 µs tick (5 kHz)**. [I, medium-high] This assumes the bootloader leaves CTRL.CLKSOURCE=0. Supporting evidence: the HID timestamp counter (+5 ticks) then runs at 1 ms, and the velocity range comes out at ~3–50 ms. If CLKSOURCE were 1, the tick would be 25 µs. Below, "tick" means one SysTick period. The SysTick ISR `FUN_0800a3c4` increments a 16-bit counter `S+0x18` every tick (S = 0x2000171E, main state block).

---

## 1. Keybed

**Scan** (`FUN_08009720`, run once per tick; the gate is `S+0x14`, set by the scan and cleared by SysTick) [P]
- 16 rows are selected by {PB14, PB2, PB1, PB0}. Rows 0–7: PB14=1, address = row. Rows 8–15: PB14=0, address = row−8.
- After a ~30-iteration settle loop the firmware reads GPIOC_IDR[15:8]. It then drives PC8–15 high (CRH=0x33333333) and returns them to input pull-up (CRH=0x88888888) before the next row.
- Data is active low. Debounce: a row value must repeat on 3 consecutive scans. The stored value is the inverse (1 = closed) at 0x200007E8[row]. Old row values are at 0x200007A8–0x10, change detection is in `FUN_08009980`.
- Rows 0–7 are the keybed. Rows 8–10 are re-read every 10th scan as the button matrix (§2). Rows 11–15 are read but never used.

**Two contacts** (`FUN_08009980`, param bit1) [P]
- `pair = row>>1`, `contact = row&1`. Even row = first contact (closes first, opens last). Odd row = second contact.
- Key index = `table_0x0800BDC8[(row>>1)*8 + bit]` = `31 − (pair*8 + bit)` (table at file 0x8DC8, 64 bytes; entries 32–63 are 0xFF).
  - Key 0 (lowest, F) is at rows 6/7, PC15.
  - Key 31 (highest, C) is at rows 0/1, PC8.

Per-key state at `S+0x1B+k`:

| event | state change | action |
|---|---|---|
| contact1 closes, state 0 | → 1 | t = tick (0x200007A8[k], u16) |
| contact2 closes, state 1 | → 0x82 (note-on pending) | raw = tick − t |
| contact2 opens, state 2 | — | t = tick |
| contact1 opens, state 2 | → 0x80 (note-off pending) | raw = tick − t |
| contact1 opens, state 1 | → 0 | (half press, nothing sent) |

**Velocity** [P]
- `raw = Δticks + 6` (white key) or `+12` (black key), clamped to 255.
- The black-key flag table is 0x0800BE70 (file 0x8E70), 32 bytes, pattern F,F#,G,…
- On-velocity is stored at `S+0x3B+k`, release raw at `S+0x5B+k`.
- MIDI velocity = `127 − T[curve*256 + raw]`. T is at **0x0800BE90** (file 0x8E90), 8 curves × 256 bytes, monotonic.
  - Curves 0–6: raw ≤ 20 → 127, raw 255 → 1.
  - At raw 64: curve 0 gives 94, curve 3 gives 62, curve 6 gives 21.
  - Curve 7 = constant 127 (fixed velocity).
- The curve is `settings[0x2C]` (default 3), forced to 7 when Fixed-Vel is on (`S+0x7B`). The curve is latched per note at note-on.
- Release velocity uses the same table (`FUN_0800421c`).
- At a 200 µs tick, raw 20 ≈ 2.8 ms between contacts and raw 255 ≈ 50 ms. [I]

**Note numbers and output** (`FUN_0800421c`) [P]
- `note = (octave−1)*12 + k − 7`. Octave is `settings[0x2E]`, range 1..10, default 5 → key 0 = MIDI 41 (F2), key 31 = 72.
- Notes outside 0..127 are dropped.
- Channel = `settings[0x2B]` (0–15). Note, channel and on-flag are latched per key at note-on, so note-off is correct after an octave change.
- USB-MIDI packets (cable 0): note-on `09 9n nn vv`, note-off `08 8n nn rv`. They are batched and queued with `FUN_08007618` (ring 0x200010AB, 960 B) and sent on EP 0x82 by `FUN_080053f8`/`FUN_08007358`.
- Keys always go to MIDI. Key events are not in the HID report.

---

## 2. Buttons

**Scan** (`FUN_08009720` tail, every 10th key scan; debounce = 4 equal samples) [P]
- Groups 0–2 = matrix rows 8, 9, 10 (PB14=0, address 0/1/2) read on PC8–15.
- Group 3 = `GPIOA_IDR&0xF` + PB13 (bit4) + !PB12 (bit5).
- Stored inverted at 0x20000147+g.
- Bits map through 0x0800BE48 (file 0x8E48) to button index, kept at `S+0x7D+idx` (0x63 = unused).

| group | bit0 (PC8/PA0) | bit1 | bit2 | bit3 | bit4 | bit5 | bit6 | bit7 (PC15) |
|---|---|---|---|---|---|---|---|---|
| 0 (row 8) | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | – | – |
| 1 (row 9) | 0x10 | 0x11 | 0x12 | 0x0C | 0x0D | – | 0x0E | 0x0F |
| 2 (row 10) | 0x06 | 0x07 | 0x08 | 0x09 | 0x0A | 0x0B | 0x14 | 0x13 |
| 3 (PA0-3, PB13, PB12) | 0x16 | 0x15 | 0x17 | 0x18 | 0x19 (PB13) | 0x1A (PB12) | – | – |

**Identities proven by code** (`FUN_08003ce0`, `FUN_080073a0`, `FUN_08007228`) [P]
- idx 0x00 = **Shift**. It is a modifier: the other actions below are blocked while it is held. Its state goes to `S+0x9A` and changes the Oct− LED.
- idx 0x0E / 0x0F = **page −/+** (knob page `settings[0x2D]`, 0..3, wraps). Their LEDs 0x0E/0x0F light in MIDI mode.
- idx 0x13 / 0x14 = **Octave − / +** (1..10). Pressing one while holding the other resets the octave to 5. The LEDs 0x13/0x14 show below/at/above the default octave.
- **Shift + idx 0x13 toggles Fixed Velocity** (`S+0x7B`). While Shift is held, the Oct− LED shows its state (0xFF on, 0x1E off).

**Inferred** [I]
- idx 0x15–0x18 (PA0–3) = the four 4D-encoder directions.
- idx 0x19 (PB13) = 4D-encoder push.
- idx 0x1A (PB12) = pedal-jack plug detect.
- idx 0x00–0x14 = the 21 backlit buttons (21 LEDs, and the LED index equals the button index for 0x0E/0x0F/0x13/0x14).
- The remaining identities (Scale, Arp, Undo, Quantize, Auto, Loop, Metro, Tempo, Play, Rec, Stop, Mixer/Browser/Plug-in/Track…) are not distinguishable in code. In stock firmware they produce no MIDI; they only set bits in HID report 0x01.
- Boot combos (`FUN_0800b20c`): row-9 pattern 0xC0 (idx 0x0E+0x0F) held at power-up → state 3 (LED/OLED test loop, `FUN_0800b15c`). Pattern 0x07 (idx 0x10+0x11+0x12) → state 5 (OLED "Firmware Ver.0.4.5", `FUN_08005c5c`).

---

## 3. 4D encoder and the 8 knobs

**4D encoder rotation** [P]
- PC6/PC7 are polled every tick and debounced (one repeat). `FUN_08009980` param bit0:
  - an edge on A while A==B → dir 1;
  - an edge on B while A==B(old) → dir 2.
- `FUN_08003e44` counts every edge: dir 1 → `S+0x16`+1, dir 2 → −1, mod 16.
- The 4-bit counter goes in HID report byte 24 (low nibble). No MIDI. Which direction is CW is unknown. [I]
- Push and directions: see §2 (idx 0x15–0x19, inferred).

**Knob rotation = endless dual-wiper potentiometers on the ADC** [P]
- `FUN_08003884`: ADC1 scan of 5 ranks: ch4, ch5, ch6, ch7, then ch14 or ch15. Sample time 239.5 cycles, software start.
- DMA1 ch1 transfers in circular 16-bit mode to 0x2000036E.
- `FUN_080033a4` (main loop, on DMA TC1): sums 4 conversions, stores the average, then advances the mux state 0..3:

| state | PB8 | PB9 | PA4/PA5 → knob | PA6/PA7 → knob |
|---|---|---|---|---|
| 0 | 0 | 0 | 0 | 4 |
| 1 | 0 | 1 | 2 | 6 |
| 2 | 1 | 0 | 1 | 5 |
| 3 | 1 | 1 | 3 | 7 |

- So knob = PB8 + 2·PB9 (+4 for the PA6/PA7 group). The pair → knob table is 0x0800BE68 (file 0x8E68): `00 04 02 06 01 05 03 07`.
- Each knob has two wipers (even ADC slot = A, odd = B). The 16 values are at 0x20000822, the previous values at 0x20000802.
- Decoding (`FUN_08003514`), 12-bit values:
  - A step is counted only when both wipers moved more than 38 counts (0x26).
  - `steps = min(|ΔA|,|ΔB|)/38`, clamped to 127.
  - Direction from the quadrant:
    - both rising → dir = (B < A);
    - A rising, B falling → dir = (max(A,B) < 0x900);
    - both falling → dir = (A < B);
    - A falling, B rising → dir = (min(A,B) > 0x700).
  - Result goes to `S+0x9C+knob`: bit7 set = increment (as used in `FUN_0800432c`), low 7 bits = steps.
- `FUN_0800432c` (consumer) [P]:
  - HID mode: `S+0xA4+2k` += steps·8, wraps in 0..998 (HID usage range 0..999).
  - MIDI mode: per-page configuration (see §7); the value moves ±1 per event between min and max.

**Knob touch** [P]
- A capacitive touch chip on bit-banged I²C **bus C (SCL PB10, SDA PB11)**.
- Init `FUN_0800a470` probes up to 10× with two address variants:
  - Variant 1, 7-bit **0x15**: writes reg/val pairs AF←3A, 1F←00, 12←18, 1E←E0, 1F←01.
  - Variant 2, 7-bit **0x50**: no init writes.
- Poll `FUN_0800a5a4` (round-robin with the strips, one bus every ~10 ticks): write the register (0x22, or 0xA0 for variant 2), then read 1 byte.
  - The byte is the **8-bit touch mask**, debounced (2 equal reads), stored at `S+0xEC..0xF3`.
  - Touch shows the knob's assignment on the OLED (`FUN_08008098` mode 1) and sets HID report bits 25–32.
- No reset line, no firmware download for this chip.

---

## 4. Touch strips (pitch, mod)

Proven: they are **not ADC**. Each strip is a capacitive touch IC on its own bit-banged I²C bus:

| strip | bus | SDA / SCL | reset | result |
|---|---|---|---|---|
| Pitch | A | PC0 / PC1 | PA10 | `S+0xD8` |
| Mod | B | PC2 / PC3 | PA9 | `S+0xDC` |

**Runtime read** (`FUN_0800a5a4`) [P]
- Write `{addr 0x15, reg 0x00}`, then read 3 bytes `n, p1, p2` from 7-bit address 0x15.
- `p = (p1==0 && p2==0) ? 0 : (n>1 ? p2 : p1)`.
- `pos = p<58 ? (p?1:0) : min(200, (p−57)*6/5)`. 0 = untouched.

**MIDI** [P]
- Value `v = pos<3 ? 0 : (pos−2)*127/198`. Untouched pitch → target 64. Untouched mod → holds its last value.
- The output slews toward the target by ±1 (difference < 20) or ±5 per update.
- Pitch (`FUN_08004c74`): `0E En LSB MSB`, with MSB = v and LSB = 0x7F when v==127 (else 0). Channel = `settings[0x1D]`.
- Mod (`FUN_0800499c`): `0B Bn cc v`. Channel = `settings[0x1E]`, CC = `settings[0x1F]` (default 1).
- Both are gated by `settings[0x1C]` bit0 (MIDI) / bit1 (HID: position is placed in report bytes 27–28 and 33–34, each preceded by a 1 ms timestamp).

**Boot init** (`FUN_08008ef8`/`FUN_08008f84` for A, `FUN_08007a64`/`FUN_08007af0` for B) [P]
- Hold SDA/SCL low and pulse reset low.
- Send raw bytes `67 4A` without ACK (`FUN_08008bfc`/`FUN_08007768`).
- Talk to an ISP protocol: writes framed as `02 02`, `80 D2 FF 00 00 00 00`, `70 80`; reads from address byte 0x81.
- If the reply shows `'5'`/0xE1, it skips ahead. Otherwise, if at least 2 of 3 checksum-like probes pass, it uploads **8192 bytes from 0x0800CAC9 (file 0x9AC9)** in 64×128-byte blocks (`FUN_08008d18`/`FUN_08007884`, frame ID from 0x0800F710/F714) and verifies.
- After that: release reset and drop the bus delay from 0x12 to 2.
- On failure, a flag is set in `S+0x0E` (bit0 A, bit1 B, bit2 knob-touch). `FUN_0800a470` then draws the error bitmap 0x0800B94A and loops forever while kicking the IWDG.
- Chip identity unknown. [I] The I²C slave address 0x15 and the downloadable-firmware pattern suggest a small touch MCU.

---

## 5. Button LEDs

**Driver**: IS31FL3236-type, 36-channel constant-current PWM LED driver on bit-banged I²C (PB6 SCL, PB7 SDA), 8-bit address **0x78** (7-bit 0x3C). [P register usage; I chip name]
- Init `FUN_080071a4`: reg 0x4F←0 (reset), reg 0x00←1 (normal), regs 0x26..0x49 ← 0x07 ×36 (on, I_MAX/4), reg 0x25←1 (update).
- Update `FUN_08007228`: PWM regs 0x01..0x15 ← 21 bytes (copy at 0x200016F4), then reg 0x25←1. Only channels 0–20 are used.
- PA15 high is required (LED power or SDB, see pin table).

**LED index → driver channel** (0x0800BD4A, file 0x8D4A; register = channel+1) [P]

```
LED idx : 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14
channel : 00 01 02 03 04 05 06 07 08 0B 0C 0D 09 0A 0E 0F 12 11 10 13 14
```

**Brightness** (`FUN_08007120(idx, val)`) [P]
- `val>>2 == 0` → off. Otherwise `PWM = table_0x0800BD5F[ch*4 + (val&3)]`.
  - Levels 0–1 = 0x1E (dim), levels 2–3 = 0xFF (bright).
  - Channel 11 gets −5 (0x19/0xFA). Channel 12 gets +0x3C (0x5A) on the dim level.
- `idx 0x15` = set all 21 PWM bytes to `val` raw. `idx 0x16..0x2A` = raw PWM for channel idx−0x16 (used by the boot sweep `FUN_080072ec`, table 0x0800BDB3).
- Host control: HID OUT report 0x80 (21 bytes, one per LED index, 0..127; e.g. 0x7C = dim, 0x7F = bright). If the first bytes are `AA AB AC`, it sets only LED[byte3] = byte4.
- LED index ↔ button: proven equal for 0x0E, 0x0F, 0x13, 0x14. For the rest it is inferred (§2).

---

## 6. Pedal input

- A TRS jack on **PC4/PC5**, plus plug detect on **PB12** (idx 0x1A). [P function; I tip/ring assignment]
- Each ADC frame drives one contact high and samples the other (`FUN_080033a4`):
  - even mux states: PC5 high, sample PC4 (ch14) → slot 0;
  - odd states: PC4 high, sample PC5 (ch15) → slot 1.
  - Values at 0x20000156/0x20000158.
- Plug insertion arms a 1500-frame settle (`S+0xE0`, `FUN_08009980`). During the 6000×5-tick start-up window the resting level of each slot is sampled as the "released" polarity (`S+0xE4+i`, `FUN_08003514`).

Pedal mode = `settings[1]` (default 3; HID 0xF4 cmd 0x22 sets 1..3) [P]

| mode | behaviour |
|---|---|
| 3 | Two switches: slot0 → `S+0x98`, slot1 → `S+0x99`. Hysteresis ±0xA0 around the calibration points `settings[0x14+2i]`/`[0x18+2i]`, 50-frame debounce. Each switch uses config entry 0/1 (`settings[2..7]`, `[8..13]`): type (0 = CC, 1 = PC), channel, CC (defaults 64 and 65), mode, min, max. Switch modes (`FUN_08004a3c`) [I, semantics]: 0 = toggle, 1 = send max on press, 2 = momentary (default), 3 = step min..max. |
| 1 / 2 | Continuous (expression) on slot mode−1, scaled into entry 2 (`settings[0x0E..0x13]`, default CC 11, 0..127). |

- Calibration via HID 0xF4: 0x10 = defaults (0x0700 / 0x0300), 0x11/0x13 = capture the current reading as the upper/lower point, 0x15 = save to flash (0x4214, 0x4218), 0x16 = load.

---

## 7. USB reports and MIDI messages

Device [P] (descriptor 0x0800B37C, config 0x0800B38E):

| item | value |
|---|---|
| VID:PID | 17CC:1860, bcdDevice 0x0045, 480 mA |
| Interfaces | IAD + Audio Control (0); MIDIStreaming (1) with EP 0x02 bulk OUT 64 / EP 0x82 bulk IN 64, one cable; HID (2) with EP 0x81 int IN 64 / EP 0x01 int OUT 64, 1 ms; DFU runtime (3), attributes 0x09, wDetachTimeout 255, transfer size 64 |
| Product string | "KOMPLETE KONTROL M32" |
| Serial string | built from flash block B bytes 4–7 (`FUN_08009bd0`) |

The HID report descriptor is at 0x0800B519 (526 B), vendor page 0xFF01.

**IN report 0x01** (38 bytes incl. ID, built in `FUN_080040bc` and sent only on change):

| byte | content |
|---|---|
| 1–5 | 40 bit flags: bits 0–24 = button idx 0x00–0x18, 25–32 = knob touch 0–7, 33 = idx 0x19, 34 = idx 0x1A, 35/36 = pedal switches |
| 6–21 | 8 × u16 knob position (0..998) |
| 22–23 | pedal (low byte only is written: `S+0xE2`) |
| 24 | low nibble = 4D-encoder counter |
| 25–26 | 1 ms timestamp |
| 27–28 | pitch strip pos (0..200) |
| 29–30 | 0 |
| 31–32 | 1 ms timestamp |
| 33–34 | mod strip pos |
| 35–36 | 0 |
| 37 | lowest-note offset ((oct−1)·12−7) |

Other reports:
- IN 0x40 (30 B): declared; no sender found.
- OUT 0x80: LEDs (§5).
- OUT 0xA0 (2 B): per-subsystem routing. Byte0 bit0/1/2 → MIDI enable for pedal / strips / knobs (`settings[0x00]`, `[0x1C]`, `[0x2A]` bit0). Byte1 same bits → HID enable (bit1). Boot default = MIDI (`FUN_08004ef4`).
- OUT 0xE0 (8-byte header + 256 B): OLED blit (x0, page0, width, pages) into the 128×32 framebuffer `S+0xF7`.
- OUT 0xF4 (32 B): pedal commands (§6). Accepted only when data[1]==0 and data[2]==3.
- Feature 0xF8 (10 B): GET returns a ROM constant (0x0800B73F). SET data[8] (usage 0xE6, 0..100) sets OLED contrast = v·3 (`FUN_08007e60`).
- Feature 0xD0–0xDF: 32-byte chunks of config blocks A (0xD0–D7) and B (0xD8–DF). Handler at 0x0800A7DC in the undecompiled region.

Vendor requests: 0x10 wIndex 0/1 → save block A/B to flash; 0x12/0x13 → read/write block A/B at an offset; 0xB3. **DFU_DETACH** (class bRequest 0) → `S[0]=4`, after 2000 loops `FUN_08004d34` writes the boot magic and resets. [P, disassembly 0x0800AAF0/0x0800AB16]

**MIDI out** (cable 0) [P]

| source | message |
|---|---|
| keys | note on/off (§1) |
| knobs, MIDI mode | per page p (0..3) and knob k: 6-byte entry at 0x20001BD2 + p·48 + k·6 = {type 0 = CC / 1 = Program Change / 2 = off, channel, CC#, mode 0..2, min, max}. Defaults: CC 14+k, ch 0, 0..127. Sends `0B Bn cc val` or `0C Cn val 00` (`FUN_0800432c`). Incoming CCs from the host update matching knob values unless the knob is touched (`FUN_0800b0dc`). |
| strips, pedal | see §4 and §6 |
| buttons, 4D encoder | none |

**MIDI in / SysEx** (`FUN_0800a204`, `FUN_08004584`) [P]
- Identity request `F0 7E 7F 06 01 F7` → `F0 7E 7F 06 02 00 21 09 60 18 20 00 05 04 00 00 F7`.
- NI SysEx `F0 00 21 09 60 18 'M' 'C' xx yy cmd …` (reply cmd+0x40): 1 = write template (name, knob config 0xC0, names 0x140), 2 = read template, 3 = write globals, 4 = read globals, 5 = save to flash.

---

## 8. Persistent storage (flash, 1 KB pages)

Flash write path [P] (`FUN_080054b8`, `FUN_08005928`, `FUN_08005528`):
- The base address is **0x0801B800 + offset** (FUN_08005928 adds the base for offsets < 0x4400).
- A single 1 KB RAM cache (0x20000378) holds the page being edited. It is flushed 300×5 ticks after the last write.
- A page is erased and reprogrammed only when its contents differ, and is verified afterwards.

| flash addr | offset | content |
|---|---|---|
| 0x0801B800 | 0x0000 | Knob names: 32 × 10 chars at `k*40 + page*10` → RAM 0x20001C92 (defaults "Knob11".."Knob84" at ROM 0x0800C690); byte 0x3FF = 0x01 marks the page initialised |
| 0x0801F800 | 0x4000 +0x000 | Block A (256 B) → RAM 0x20000EAB; byte 4 → pedal mode at boot; default ROM 0x0800C7D0 |
| | +0x100 | Block B (256 B) → RAM 0x20000FAB; bytes 4–7 = serial number; default ROM 0x0800C8D0 (serial FF FF FF FF) |
| | +0x200..0x238 | Global settings (0x39 B) → RAM 0x20001B98, layout below; default ROM 0x0800C9D0 |
| | +0x239..0x2F8 | Knob templates, 4 pages × 8 × 6 B → RAM 0x20001BD2; default ROM 0x0800CA09 |
| | +0x3A7 = 3, +0x3A8..3B7 | Copy of the USB serial string; +0x3BC..3E3 = product string; +0x3F8 = `62 18 45 00` (purpose unknown) |
| | +0x3FC / 3FD / 3FE | Layout versions 3 / 1 / 1. A mismatch resets that section; 0xFF at 0x3FC resets everything; the calibration fields are kept |
| 0x0801FC00 | 0x4400 | Bootloader request: magic `55 5A A5 AA 59 95 3A A3` followed by 0xFF (`FUN_08003a08`) |

Global settings struct (RAM 0x20001B98, flash +0x200) [P]:

| offset | field |
|---|---|
| 0x00 | pedal routing flags |
| 0x01 | pedal mode (1..3) |
| 0x02..0x07, 0x08..0x0D, 0x0E..0x13 | pedal entries 0/1/2 {type, channel, CC, mode, min, max} |
| 0x14/0x16 | calibration "high" for slots 0/1 (u16) |
| 0x18/0x1A | calibration "low" for slots 0/1 (u16) |
| 0x1C | strip routing flags |
| 0x1D | pitch-bend channel |
| 0x1E | mod channel |
| 0x1F | mod CC |
| 0x20..0x29 | 5 × u16 (A0 08 40 06 A0 03 00 08 50 04); kept across reset, no reader found |
| 0x2A | knob routing flags |
| 0x2B | keyboard channel |
| 0x2C | velocity curve |
| 0x2D | knob page |
| 0x2E | octave |
| 0x2F..0x38 | template name ("Template 1") |

---

## Other confirmed details

- **OLED init** (`FUN_08007f6c`): AE 40 B0 C0 81 E0 A0 A6 A8 1F D3 00 D5 F0 D9 22 DA 02 DB 49; then 8D 14 AF (`FUN_08007ee4`).
  - Off: 8D 10 AE (`FUN_08007eb4`).
  - Pixels are stored inverted (written as 0xFF−b).
  - SPI1 runs master, TX-only bidirectional mode, CPOL=1/CPHA=1, /16 (4.5 MHz).
- **USB suspend**: after 0xFFF0 main-loop passes in suspend → OLED off and PA15 low. Resume restores both.
- **Main-loop state machine** `S[0]` (`FUN_0800b31c`): 0 = normal, 2 = suspended, 3 = test loop, 4 = bootloader countdown, 5 = version screen.
- **Bus timing**: every I²C clock-stretch wait (buses A/B/C) is unbounded. A stuck chip hangs the MCU until the IWDG fires.

---

## Open questions / needs hardware test

1. SysTick rate: confirm 200 µs (CLKSOURCE=0 after the bootloader). It sets the velocity time scale.
2. Physical identity of button idx 0x01–0x0D, 0x10–0x12 and of the 4D-encoder directions/push (idx 0x15–0x19). Method: log HID report 0x01 bits, or GPIO state on a test build.
3. Encoder rotation sense (which of dir 1/2 is clockwise) and edges per detent (the counter counts every edge).
4. Knob numbering vs. panel position (assumed knob 0 = leftmost) and knob rotation sense.
5. PB12: whether "high = plug inserted" is correct. Which of PC4/PC5 is tip and which is ring.
6. PA15: LED driver SDB pin, or a LED supply switch.
7. Matrix decoder topology (two '138s, or one 4→16) and what sits on rows 11–15 (unused by the firmware).
8. Touch-strip and knob-touch controller part numbers. Whether the 8 KB blob at 0x0800CAC9 is uploaded on every boot or only when the chip reports no firmware. A replacement firmware must either keep this upload path or rely on chips that already hold their firmware.
9. Physical meaning of the strip report bytes `n, p1, p2` (multi-touch?) and the strip direction (which end is 0).
10. HID IN report 0x40 and settings fields 0x20..0x29: no code use found. They may be dead or handled in the undecompiled region 0x0800A764–0x0800AC74.
11. The LED index ↔ button mapping beyond 0x0E/0x0F/0x13/0x14. Test with OUT report 0x80, one LED at a time.
