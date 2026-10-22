# My Kontrol manual

The keyboard has three modes, and it picks one by itself:

| Mode | When | Badge |
|---|---|---|
| **Standalone** | no DAW is talking to it | none |
| **Mackie** | a DAW speaks Mackie Control on the DAW port (Logic, Live, Cubase, Reaper, ...) | `MCU` |
| **Live** | the My Kontrol script runs in Ableton Live | the Live views |

**System > DAW Control > Off** keeps it in standalone mode whatever the computer does.

## USB MIDI ports

| Port | Carries |
|---|---|
| **My Kontrol Keys** | keys, Smart Play output, knob CCs, strips, pedals; MIDI clock in and out |
| **My Kontrol DAW** | control only: Mackie Control or the Live script. Never record from it. |

## Buttons in every mode

| Button | Press | Shift + press |
|---|---|---|
| Scale | scale on/off (remembers quantize or easy) | Scale & Chord menu |
| Arp | arpeggiator on/off | Arp & Repeat menu |
| Octave − / + | octave down / up (±3) | −: fixed velocity on/off, +: split on/off |
| Octave − and + together | reset octave and transpose, all notes off | |
| Browser | menu (Live: clip launcher) | menu |
| Tempo | tap tempo (in Live mode: Live's tap tempo) | Tempo light off |

## Standalone

| Control | Does |
|---|---|
| Quantize | note repeat: tap toggles, hold = only while held |
| Ideas | chord memory: hold Ideas and play a chord to teach it; tap toggles it |
| Preset ↑ / ↓ | knob page 1-4 |
| Play / Stop | MIDI Start / Stop on the Keys port (when Clock Out is on) |
| 4D push | menu |
| 4D turn, up / down | home page: play info / knob values |
| 4D left / right | transpose −/+ |
| Knobs | CCs of the current page; the screen shows value and CC |

The home screen shows channel, octave, transpose and split on top. Below that
is the scale, or the last note played, with a velocity meter on the right.
The bottom line shows what is shaping the notes: arp, repeat, chord and strum.

## With a DAW (Logic, Live)

Logic and other DAWs connect through Mackie Control. In Logic, go to
Control Surfaces > Setup, choose New > Install > Mackie Control > Scan, and
set both ports to **My Kontrol DAW**. Ableton Live connects through the My
Kontrol script; see [daw/ableton](../daw/ableton/README.md).

Both DAWs work the same way.

**Knobs.** **M** and **S** step through what the knobs control. The screen title shows
the current mode, and each knob shows its name and a position bar. Touch a knob
to see its full name and value.

| Mode | Knobs control | Preset ↑ / ↓ |
|---|---|---|
| Instrument | the selected track's instrument (in Live, a Rack's macros come first) | parameter bank |
| Plug-in | Logic: plug-ins of the selected track. Live: the selected device (Shift + Preset: next device) | parameter bank |
| Volume | volume of 8 tracks | tracks 1-8, 9-16, ... |
| Pan | pan of 8 tracks | tracks by 8 |
| Sends | sends of the selected track | |

- **Plug-in** jumps to Instrument (Shift: Plug-in).
- **Track** jumps to Volume (Shift: Sends).
- The keyboard remembers the mode.
- Controls > M/S Mode 1-5 sets the order, or switches modes off.

**4D encoder.** It works as a keyboard on whatever window has focus: tracks
and regions in the arranger, notes in the piano roll, the browser.

| 4D | Does |
|---|---|
| Turn | next / previous item (→ / ←) |
| Shift + turn | zoom |
| Up / down / left / right | arrow keys (Shift: extend the selection) |
| Push | Return |
| Shift + push | Space (play / stop) |

The first time you plug it in, macOS shows the Keyboard Setup Assistant; close it.
**System > 4D as Keys > Off** puts the 4D back on the DAW protocol.

**Buttons**

| Button | Press | Shift + press |
|---|---|---|
| Play / Stop / Rec | play / stop / record | Play: back to start; Rec in Live: session record |
| Loop, Metro | cycle, click | |
| Undo | undo | redo |
| Quantize | quantize the selection (Logic: Q; Live: the selected clip) | |
| Ideas | Logic: capture as recording; Live: capture MIDI | |
| M / S | knob mode − / + | mute / solo the selected track (Logic: first track of the bank) |
| Browser | Logic: menu; Live: clip launcher | menu |

The lights follow the DAW.

## Menu

Open the menu with 4D push, or with Browser (Shift + Browser in Live mode).

- Turn or press up/down to move through the list.
- Push to change a value; turning then edits it, and pushing again finishes.
- On/off settings flip at a push.
- 4D left goes back.

**Scale & Chord**

| Setting | Values |
|---|---|
| Scale | off, quantize (wrong notes snap to the scale), easy (white keys play the scale) |
| Root, Type | 18 scales |
| Chord | off, octave, fifth, triad, 7th, 9th, learned (diatonic when a scale is on) |
| Learn Chord | hold a chord, then push |
| Strum | 0-200 ms, direction up, down or alternate |

**Arp & Repeat**

| Setting | Values |
|---|---|
| Arp, Mode | up, down, up/down, order, random, chord |
| Rate | 1/4 to 1/32, triplets |
| Octaves, Gate, Swing, Hold | |
| Note Repeat, Repeat Vel | velocity from the key or from the mod strip |
| Tempo | 40-240 BPM |
| Clock | internal or external MIDI clock |
| Clock Out | sends MIDI clock on the Keys port |

**Keys**

| Setting | Values |
|---|---|
| Channel, Octave, Transpose | |
| Velocity | soft, linear, hard, fixed; Fixed Vel sets the value |
| Split, Split At, Lower Ch, Lower Oct | keys below the split point play on their own channel and octave, without Smart Play |
| All Notes Off | |

**Controls**

- Knob Page and Knob 1-8 CC.
- Knobs in DAW: DAW, or MIDI CC to keep the knobs on CCs even when a DAW is connected.
- Mod Strip CC, Pedal Tip CC, Pedal Ring CC.

**Presets**: 8 slots holding all settings. Pick a slot, then Load or Save.

**System**

| Setting | Values |
|---|---|
| DAW Control | auto, off |
| Tempo Light | auto, always, off |
| Light Level | brightness of unlit buttons |
| Contrast, Screen Saver | |
| Update Mode | waits for the web installer |
| Reset All | |
| About | |

## The Tempo light

| Setting | Flashes |
|---|---|
| Auto | while the arpeggiator or note repeat is playing, with incoming MIDI clock, with the DAW's beat, or after you tap a tempo |
| Always | on every beat of the internal tempo |
| Off | never |

Shift + Tempo stops flashing that started from a tap.

## Updating and recovery

- **Update:** use the [web installer](https://my-kontrol.vercel.app), or System > Update Mode.
- **Recovery:** hold **Shift + Browser** while plugging in, then flash again.
- **Crash protection:** the keyboard restarts after a crash. After three crashes in a row it waits in update mode.
- **Settings and presets** survive updates. A version that changes the settings format resets them once.
- **Back to NI's firmware:** use the installer's Restore. NI's settings, calibration and knob names were never touched.
