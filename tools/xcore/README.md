# Open XS1 toolchain

Builds XMOS XS1 (xCORE, Komplete Kontrol MK1) firmware from C without XMOS's tools.
Plain Python 3, no dependencies. Compiler: LLVM's XCore backend (`clang --target=xcore -S`),
which emits assembly but has no assembler or object writer; the rest is here.

| file | what |
|---|---|
| `xdis.py` | disassembler; its opcode table drives the assembler too (owned by the xmos-re work) |
| `xas.py` | assembler: clang's XS1 assembly → relocatable object (JSON) |
| `xld.py` | linker: objects → one tile RAM image + map; gc of unreferenced `.cc_top` blocks; picks every instruction size (relaxation) |
| `ximage.py` | `--check` validates any xflash upgrade image (header, segment table, CRCs, padding); rebuilds NI's image locally. Our images come from XMOS's xflash |
| `xcc.py` | cc-style driver (`-c`: clang to IR, the stack-alignment fix below, llc, xas) so the firmware Makefile builds XS1 objects |
| `xsim.py` | XS1 simulator: tiles, 8 hardware threads each, timers, ports, clock blocks, channels (also between tiles), locks, synchronisers, events |
| `s49run.py` | bring-up harness for our S49 firmware: runs our tile images, shows displays and lights, injects input, captures MIDI; `--stdio` for scripted tests |
| `s49emu.py` | Komplete Kontrol S49 MK1 board: both tiles plus LCD controllers, LED chains, input registers, I²C sensor MCUs, keybed UART, DIN MIDI, SPI flash |
| `xtest.py` | tests, below |
| `test/` | test program, and a small libc (printf, str*) used only to run the host tests in xsim |
| `../../firmware/platform/xs1/` | `crt0.s` (startup), `rt.c` (compiler helpers), `platform.c` (`main` → `mk_main`), `tile.ld` (link description) |

## How the pieces fit

```
core/*.c --clang -S--> *.s --xas--> *.o --xld (one run per tile)--> tile0.bin tile1.bin --xflash (XMOS)--> upgrade.bin
                                                                                ximage --check validates it
```

- **Assembly**: everything that does not depend on a symbol is encoded at once. Instructions
  with symbolic operands (branches, `bl`, `ldap`, `ldw r, cp[x]`, `ldaw r, dp[x]`, ...) stay
  symbolic in the object with every encoding that could hold them (short, prefixed).
- **Link**: per tile, code from 0x10000 (entry first), then the cp area (`_cp`: `.cp.*`
  constants and strings), then the dp area (`_dp`: `.dp.data`, then `.dp.bss`). The image ends
  at `_bss_start`; the stack grows down from `_stack_top` (RAM top − 8). The linker then lays
  everything out with all instructions short and grows the ones that do not fit until nothing
  changes. `.jmptable` entries out of short range go through a long `bu` placed after the table.
- **Startup** (`crt0.s`): sets cp, dp, sp, kep (exceptions park in `_trap`), zeroes bss,
  calls `main(0, 0)`; return parks the thread in `_exit`.
- **Compiler helpers** LLVM XCore calls: `memcpy`, `__memcpy_4`, `memmove`, `memset`,
  `__misaligned_load/store`, `__ashldi3`, `__lshrdi3`, `__ashrdi3`, `__udivdi3`, `__divdi3`,
  `__umoddi3`, `__moddi3` (all in `rt.c`). Float is not covered (`__mulsf3` etc.).

## Make

```make
XCORE   = ../tools/xcore
XCC     = /opt/homebrew/opt/llvm@22/bin/clang --target=xcore
XCFLAGS = -Os -std=gnu11 -ffreestanding -fno-builtin -Wall -Wextra -Icore -Iusb -Iboards/$(BOARD)

$(BUILD)/xs1/%.s: %.c
	@mkdir -p $(dir $@)
	$(XCC) $(XCFLAGS) -S $< -o $@
$(BUILD)/xs1/%.o: $(BUILD)/xs1/%.s
	python3 $(XCORE)/xas.py $< -o $@
$(BUILD)/tile0.bin: $(TILE0_OBJS)
	python3 $(XCORE)/xld.py @platform/xs1/tile.ld $^ -o $@ -m $(BUILD)/tile0.map
$(BUILD)/tile1.bin: $(TILE1_OBJS)
	python3 $(XCORE)/xld.py @platform/xs1/tile.ld $^ -o $@ -m $(BUILD)/tile1.map
$(OUT).bin: $(BUILD)/tile0.bin $(BUILD)/tile1.bin
	# package the tiles with XMOS's xflash, then:
	python3 $(XCORE)/ximage.py --check $@
```
(`TILEn_OBJS` include `$(BUILD)/xs1/platform/xs1/rt.o`; `tile.ld` adds `crt0.s`.)
`xld.py` also takes `.s` files directly. `-g` works (debug sections are dropped).
LLVM's XCore backend miscompiles stack objects aligned below 4 bytes (a char array, a byte
whose address is taken): their frame offset is divided by 4 for `ldaw r, sp[n]` and the
remainder dropped, so two of them can overlap. Seen at `-O0` in the host tests and at `-Oz`
with two `uint8_t[10]` buffers. `xcc.py` (and xtest) work around it: C goes to LLVM IR, every
`alloca` gets an alignment of at least 4, then `llc`. With that, `-O0` to `-O3`, `-Os` and
`-Oz` all pass the simulator tests. The firmware builds with `-Oz` (platform.mk): 12 %
smaller than `-Os`, which matters in 64 KB per tile.

## Tests: `python3 tools/xcore/xtest.py [roundtrip|core|image|sim]`

- `roundtrip`: disassembles both segments of NI's S49 image (`fw/kks/s49_xmos_120.bin`, only
  if present locally), reassembles with labels for every branch target and lets the relaxation
  pick the sizes: byte-identical. Long forms with a value that would fit the short form
  (NI's linker left 430 of them) are written with the `.l` suffix; undecodable data words and
  28 non-canonical `bt r, 0` data words are written as `.short`.
- `core`: compiles `firmware/core/*.c`, links one tile with the runtime (board/hal functions
  stubbed by `--stub-undefined`), prints sizes, and cross-checks every linked instruction
  against clang's assembly by decoding it with xdis.
- `image`: the test program (`test/prog.c`) for two tiles → upgrade image → `--check` (a
  format test only: word 0x04 is 0, which the boot loader rejects).
- `sim`: runs `test/prog.c` and the firmware host tests (test_smartplay, test_core,
  test_host) in xsim and compares their output with the same sources built for the Mac.
- `s49`: builds `test/s49_tile0.c` + `test/s49_tile1.c` (a minimal LCD/LED driver and both
  sides of the tile link) and runs them in the harness, with both tiles and with tile 0 alone.
- `fw`: `make BOARD=s49 DEV=1` (our firmware), then the harness with both tiles: splash,
  home screen, CC page, a knob's CC on DIN OUT, the menu; then with the HostLink stub and USB
  connected: a key on DIN and USB, a knob turned on sensor 0x12, Scale on lighting the light
  guide (about 1.5 minutes: the firmware's bit-banged I2C is most of the emulated work).
- `sim` also runs test_seg and test_panel (the display and panel drivers) in xsim.
- `emu` (opt-in, about 1 min, needs NI's image locally): boots NI's S49 firmware on the
  board model and checks the LCD init sequence, the standalone display text and that a key
  press gives a note on DIN MIDI OUT.

## Not known / not done

- Header word 0x04 of the upgrade image: per-model value in NI's images, checked by the S49
  boot loader (0, 0x7FFFFFFF, 0xFFFFFFFF are rejected), not by libflash. Our images are
  packaged with XMOS's xflash; `ximage.py` is a checker and rebuilds NI's image locally
  (`--id` with the original word). Its default word 0 does not boot.
- Segment order/flags: segment 0 gets 0x80000002, the rest 2, as in NI's images; which segment
  lands on which physical tile is inferred, not proven.
- Where the stage-2 loader itself sits in RAM while loading (an image that is too large could
  overwrite it) is not known; NI's largest segment is 53 988 bytes.
- xsim: no interrupts, kernel calls, xCONNECT switch registers or ready/strobe signals;
  timing is per instruction (4 cycles per thread, 500 MHz), not the exact pipeline. No XS2;
  no DWARF output.

## Board emulator: `python3 tools/xcore/s49emu.py [IMAGE] [--ms N] [-e "at T CMD"] [--leds]`

Runs an upgrade image (default NI's `fw/kks/s49_xmos_120.bin`, local only) or two tile images
(`--tiles ui.bin usb.bin`) with the devices of `docs/hardware/s-mk1.md` attached, then prints
the LCD command log, the 9 displays as text (16-segment cells decoded with the font table),
LED brightness per chain bit, DIN MIDI OUT bytes, the I²C traffic and the thread states.
Script commands: `at T key NOTE [VEL]`, `keyoff NOTE`, `button BIT 0|1`, `midi HEX`,
`knob N VALUE`, `show`. Speed: about 2 M instructions/s, 3.6 s of NI's firmware in about
70 s (busy-polling loops are detected and slept). `xsim.System.save/load` snapshots a
running board (pickle), which makes repeated experiments from a booted state cheap.

What NI's firmware does in the emulator (USB not connected; the XUD thread waits forever on
its ULPI-clocked ports, the flash is erased):
- LCD: reset pulse at 0.17 ms, then `A3 A0 C0 24 81 03 2C 2E 2F AF A4` to all three
  controllers and a blank refresh at 4 ms; content at 3.16 s.
- Displays: `1/2` / `FACTORY` on display 1, `CC14` ... `CC21` on displays 2–9 (standalone
  MIDI mode, page 1). Input bit 28 or 29 switches to page `2/2` (`CC22` ... `CC29`).
- Boot MIDI on DIN: `B0 40 7F B0 41 7F B0 40 00 B0 41 00`; input bits 5 and 6 send CC64 and
  CC65 on/off (pedal-like switches on the input registers).
- Keybed frame `94 3C 58` (note 60, raw velocity 600) → DIN `90 3C 04`; key off → `80 3C 00`.
- Sensor 0x12 value 0 from 2000 to 8000 → DIN `B0 0E 7F` (CC14).
- DIN IN bytes reach the USB tile's UART (its RX counters), nothing is echoed to DIN OUT.
- I²C: boot writes of `60 …` blocks, then `03 05` + 102/22-byte reads every 10 ms from 3.1 s.

## Bring-up harness: `python3 tools/xcore/s49run.py TILE0.bin [TILE1.bin] [--stdio | --watch]`

Runs our own tile images on the board model. Without a tile-1 image, tile 1 is the HostLink
stub, so tile 0 (core and panel) can be brought up before USB works. The stub implements the
**tile link contract**, documented in `HostLink` (s49emu.py): link chanends are chanend 0 of
each tile (0x80000002 / 0x00000002), tile 1 speaks first with USB_STATE, every message is a
packet `[type, n, payload]` + END, carrying USB-MIDI packets, 5-pin MIDI bytes and flash
read/erase/write. `test/s49_tile0.c` / `s49_tile1.c` / `xs1io.h` are a worked example of both
sides.

- `--watch`: redraws when the displays or lights change: the 9 displays side by side (bar of
  the 17 extra segments, 2 lines of 8 characters), the light guide as one letter per key
  (R G B Y M C W, capital = bright), the button/strip chain as a level per bit.
- `--stdio`: commands on stdin, time moves only on `tick MS` (deterministic). `key NOTE [RAW]`,
  `keyoff`, `knob N DELTA`, `knob-set`, `btn BIT [MS]`, `btn-down/up`, `turn N`, `pedal`, `sw`, `touch`,
  `din-in HEX`, `in CABLE HEX`, `usb 0|1`, `event TYPE ID VALUE` (DEV=1 builds: an event
  for the core over the link, like tools/mktest.py), `screen`, `leds`, `threads`, `quit`. After a tick it
  prints `din-out <hex>` and `out <cable> <hex>` for MIDI sent; `stopped <reason>` on a crash.
  Buttons are by input bit until the bit map is known.

```sh
python3 tools/xcore/xtest.py s49          # builds tools/xcore/build/s49_tile0.bin, s49_tile1.bin
python3 tools/xcore/s49run.py tools/xcore/build/s49_tile0.bin --ms 20
printf 'tick 5\nscreen\nin 0 90 3C 64\ntick 2\nquit\n' | python3 tools/xcore/s49run.py tools/xcore/build/s49_tile0.bin --stdio
make -C firmware BOARD=s49 DEV=1          # our firmware
printf 'tick 1400\nscreen\nevent 2 0 1\ntick 30\nscreen\nquit\n' | \
  python3 tools/xcore/s49run.py firmware/build/s49-dev/tile0.bin firmware/build/s49-dev/tile1.bin --stdio
```
The harness reads the glyphs from `firmware/drivers/font16.c`, so the displays read back as
the characters our firmware meant.
