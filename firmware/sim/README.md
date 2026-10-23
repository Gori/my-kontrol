# M32 simulator

`mk-sim` runs the real firmware core (`firmware/core`) on macOS, as a
Komplete Kontrol M32 running My Kontrol. It talks to DAWs over CoreMIDI
virtual ports with the same names as the USB device, and shows the OLED and
the button lights in a terminal. Use it to build and test a host for the
protocol ([protocol.md](../../docs/protocol.md)) without the hardware. Mackie Control
works on the same port too.

## Build

```sh
make -C firmware sim          # firmware/sim/build/mk-sim
make -C firmware/sim probe    # firmware/sim/build/mk-probe
make -C firmware/sim check    # run mk-sim headless and probe it
make -C firmware/sim check-stdio   # scripted session over --stdio, no CoreMIDI
```

Needs the Xcode command line tools and python3 (for `tools/surface2c.py`).

## Run

```sh
firmware/sim/build/mk-sim                  # terminal UI, 130 columns or more
firmware/sim/build/mk-sim --log sysex.txt  # also log every sysex in and out as hex
firmware/sim/build/mk-sim --no-ui          # headless, for scripted tests
```

The sim starts from default settings (DAW Control = Auto) every time; the
flash is RAM.

## Keys

| Control | Keys |
|---|---|
| knobs 1–8 | `q w e r t y u i` = +1, `a s d f g h j k` = −1; hold to repeat. The knob is touched on the first turn and released 800 ms after the last |
| 4D encoder | arrows = up / down / left / right, Enter = push, `<` `>` or `,` `.` = turn −1 / +1 |
| keybed | `z x c v b n m /` = the lowest 8 keys, velocity 100, released after 300 ms (terminals report no key-up) |
| Shift | `H`: held until `H` again |
| buttons | `C` Scale, `A` Arp, `U` Undo, `Q` Quantize, `I` Ideas, `L` Loop, `O` Metro, `T` Tempo, `P` Play, `R` Rec, `S` Stop, `[` Preset up, `]` Preset down, `M` Mute, `N` Solo, `B` Browser, `G` Plug-in, `K` Track, `{` Octave −, `}` Octave + |
| quit | Ctrl-C, or Esc Esc |

A button keystroke is a 100 ms press. The light grid under the screen shows
each button's key and its light (off / dim / on). Arrow keys and Return that
the real keyboard would type as a USB keyboard (System > 4D as Keys) are shown
as "typed:" and not sent anywhere.

## Scripted tests: --stdio

`mk-sim --stdio` opens no MIDI ports and draws no UI. It reads commands from
stdin, one per line, and prints every complete message the keyboard sends as
`out <cable> <hex bytes>`, flushed per line and in order with the command
replies. Cable 0 is Keys, 1 is DAW. With `--fake-time` time stands still
except on `tick`, so a run is deterministic; without it time runs in real
time.

| Command | |
|---|---|
| `in <cable> <hex bytes>` | a MIDI message arriving, for example `in 1 F0 7D 4D 4B 03 F7`; the keyboard handles it at once |
| `tick <ms>` | run the keyboard for that long (`--fake-time` only) |
| `event <type> <id> <value>` | a control, as `app_event()`; types from `core/hal.h`: 0 key, 1 button, 2 encoder, 3 knob, 4 knob touch, 5 strip, 6 pedal |
| `screen` | `screen`, 32 lines of 128 `#` / `.`, then `end` |
| `find <text>` | `found <x> <y> <font>` if the screen shows the text in one of the keyboard's fonts (plain or `inverted`), else `notfound` |
| `lights` | `lights`, one `<control id> <level 0..255>` line per control 0–25, then `end` |
| `quit` | exit; so does the end of input |

Blank lines and lines starting with `#` are skipped; anything else unknown
answers `error ...`. The screen shows the boot splash for the first 1200 ms,
and the lights update every 10 ms of keyboard time. The keyboard answers some
messages from its main loop, for example `VIEW` after `MODES`, so tick a few
ms before expecting those. `check_stdio.sh` is a worked example:

```sh
firmware/sim/build/mk-sim --stdio --fake-time <<'EOF'
tick 1300
in 1 F0 7D 4D 4B 00 02 00 01 03 53 69 6D 03 31 2E 30 F7
event 4 0 1
find Not mapped
EOF
```

## Pointing a DAW at it

Start `mk-sim` before the DAW, or rescan MIDI ports in the DAW.

- Control surface (your host script, or Mackie Control in Logic or Ableton
  Live): input and output **My Kontrol DAW**.
- Notes: **My Kontrol Keys** as a MIDI input (and output, if the DAW echoes
  knob CCs back).

The ports have fixed unique IDs, so a DAW finds them again on the next run.

Set the ports in the DAW's own settings, not with `defaults write`. A
running app doesn't see an outside edit to its preferences, and writes its
own copy back over it.

## mk-probe

A minimal host in one C file (`probe.c`) and an example of the handshake. It
waits for `HI`, sends `HELLO` v2, checks `IDENT`, sends `DESCRIBE` and checks
that the joined `DESCRIPTION` is JSON with `"board":1`, then sends `BYE`.

```sh
firmware/sim/build/mk-probe            # exit status 0 = all ok
firmware/sim/build/mk-probe --json     # also print the surface description
firmware/sim/build/mk-probe --listen   # stay connected and print what the keyboard sends
```

It prefers a virtual port, so with both a keyboard and the sim connected it
talks to the sim. `--sim` accepts only the sim. `--port NAME` picks another port.

## Not simulated

Touch strips, the pedal, USB suspend and reconnect, the bootloader (asking
for it quits the sim), and the screen contrast.
