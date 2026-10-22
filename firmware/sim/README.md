# M32 simulator

`mk-sim` runs the real firmware core (`firmware/core`) on macOS, as a
Komplete Kontrol M32 running My Kontrol. It talks to DAWs over CoreMIDI
virtual ports with the same names as the USB device, and shows the OLED and
the button lights in a terminal. Use it to build and test a host for the
protocol ([protocol-v2.md](../../docs/protocol-v2.md),
[protocol.md](../../docs/protocol.md)) without the hardware. Mackie Control
works on the same port too.

## Build

```sh
make -C firmware sim          # firmware/sim/build/mk-sim
make -C firmware/sim probe    # firmware/sim/build/mk-probe
make -C firmware/sim check    # run mk-sim headless and probe it
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

## Pointing a DAW at it

Start `mk-sim` before the DAW, or rescan MIDI ports in the DAW.

- Control surface (your host script, or Mackie Control in Logic or Ableton
  Live): input and output **My Kontrol DAW**.
- Notes: **My Kontrol Keys** as a MIDI input (and output, if the DAW echoes
  knob CCs back).

The ports have fixed unique IDs, so a DAW finds them again on the next run.

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
talks to the sim. `--port NAME` picks another port.

## Not simulated

Touch strips, the pedal, USB suspend and reconnect, the bootloader (asking
for it quits the sim), and the screen contrast.
