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
| Browser | menu (in Live mode: clip view) | menu |
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

## Mackie (Logic and others)

In Logic, go to Control Surfaces > Setup, choose New > Install > Mackie
Control, and set both ports to **My Kontrol DAW**.

| Control | Press | Shift + press |
|---|---|---|
| Play | play | back to start |
| Stop, Rec, Loop, Metro | stop, record, cycle, click | |
| Undo | undo | redo |
| Quantize, Ideas | F1, F2 (assign in the DAW) | F5, F6 |
| Preset ↑ / ↓ | previous / next track | bank left / right |
| M, S | mute / solo of strip 1 | |
| Plug-in, Track | plug-in / pan assignment | instrument / send |
| 4D | arrows, push = Enter, turn = jog wheel | left / right = bank |
| Knobs | V-pots 1-8 (System: Knobs in DAW = DAW) | |

The screen shows the timecode and the track names. Turning a knob shows that
strip's name and value. The lights mirror the DAW.

## Ableton Live

Install the script as described in [daw/ableton](../daw/ableton/README.md).
Plug-in, Track and Browser switch between the device, mixer and clip views.
The full map is in that README.

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

- **Update:** use the [web installer](https://gori.github.io/my-kontrol/), or System > Update Mode.
- **Recovery:** hold **Shift + Browser** while plugging in, then flash again.
- **Crash protection:** the keyboard restarts after a crash. After three crashes in a row it waits in update mode.
- **Settings and presets** survive updates. A version that changes the settings format resets them once.
- **Back to NI's firmware:** use the installer's Restore. NI's settings, calibration and knob names were never touched.
