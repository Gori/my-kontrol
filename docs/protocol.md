# My Kontrol host protocol (v1)

How a DAW script talks to a My Kontrol keyboard. Used by the Ableton Live
script in `daw/ableton/`; any other host can implement it.

Everything runs on the **DAW port** (USB MIDI cable 2, "My Kontrol DAW").
The Keys port only carries music (notes, pitch bend, mod, pedals, knob CCs in
MIDI mode) and is never used for control.

Sysex messages are `F0 7D 4D 4B <cmd> <data...> F7`. Receivers ignore data
bytes beyond the ones a message defines, so messages can grow compatibly. `7D` is the
non-commercial manufacturer ID; `4D 4B` is "MK". Data bytes are 7-bit.
Strings are ASCII 0x20–0x7E with no terminator, unless a message says otherwise.

## Session

1. When USB comes up, and every 2 s while no host is connected, the device sends `HI`.
2. The host answers with `HELLO`. It may also send `HELLO` unprompted, for
   example when the script loads, and at any time, including while already
   connected. The device always replies `IDENT` and switches into host mode:
   control buttons go to the host instead of acting locally.
3. The host sends at least one message per second. `PING` is fine. After 3 s of
   silence the device leaves host mode.
4. The host sends `BYE` when it disconnects (for example when Live closes or the
   script is deselected).

After `IDENT`, the host sends its full state: every text, param, light,
transport and grid message for the current view. It sends everything again
whenever the device sends `VIEW` or `REFRESH`.

## Device → host

### Sysex

| cmd | name | data |
|---|---|---|
| 0x01 | IDENT | proto=1, board, fw_major, fw_minor, fw_patch, keys, knobs, caps, view |
| 0x02 | HI | proto=1 |
| 0x05 | VIEW | view: 0 device, 1 volume, 2 clips, 3 pan, 4 sends |
| 0x06 | REFRESH | none; the host re-sends everything for the current view |

- `board`: 1 = Komplete Kontrol M32. Future boards get the next numbers (see `docs/porting.md`).
- `caps` bits: 0 touch strips, 1 knob touch sensing, 2 4D encoder, 3 pedal jack.
- `view`: the current view, as in VIEW. Older firmware may leave it out; the host then keeps the view it had.

### Controls

All controls are on MIDI channel 16.

| Control | Message |
|---|---|
| Button | note-on, note = control ID (table below), velocity 127 = press, 0 = release |
| Knob k (0-based) turned | CC 0x10+k, relative signed-bit: 1..63 = +n, 65..127 = −(v−64) |
| Knob k touched / released | note-on 0x40+k, velocity 127 / 0 |
| 4D encoder turned | CC 0x30, relative signed-bit |

Shift is sent like any other button, so the host can build Shift combinations.
In host mode the device does not act on any button below except Shift.

The device keeps handling these itself:

- Scale and Arp, plus Shift+Scale and Shift+Arp
- Octave − and Octave +
- Shift+Browser, which opens the device menu
- the view buttons:
  - Plug-in → device view
  - Track → volume view
  - Browser → clip view
- Mute and Solo without Shift: previous / next knob mode, in the order the
  user set on the keyboard (default: device, volume, pan, sends)

Changing the view sends `VIEW`; these presses are not forwarded as notes.
Shift + Mute and Shift + Solo are forwarded as the Mute and Solo notes
(with Shift held).

The device remembers the last view across restarts and announces it in
`IDENT`.

### Control IDs

Shared by every keyboard. A board sends only the IDs it has.

| ID | Control | ID | Control | ID | Control |
|---|---|---|---|---|---|
| 0 | Shift | 9 | Play | 18 | Track |
| 1 | Scale | 10 | Rec | 19 | Octave − |
| 2 | Arp | 11 | Stop | 20 | Octave + |
| 3 | Undo | 12 | Preset ↑ | 21 | 4D up |
| 4 | Quantize | 13 | Preset ↓ | 22 | 4D left |
| 5 | Ideas | 14 | Mute | 23 | 4D right |
| 6 | Loop | 15 | Solo | 24 | 4D down |
| 7 | Metro | 16 | Browser | 25 | 4D push |
| 8 | Tempo | 17 | Plug-in | 26 | Pedal plugged (state) |

IDs 32–63 are reserved for controls on other models, such as Clear, Mixer,
Instance, MIDI, Setup, Auto, Count-in, Page ←/→ and Fixed Vel.

## Host → device

| cmd | name | data |
|---|---|---|
| 0x00 | HELLO | proto=1 |
| 0x03 | PING | none |
| 0x04 | BYE | none |
| 0x10 | TEXT | field, chars (at most 24) |
| 0x11 | PARAM | knob, value 0–127, flags |
| 0x12 | LIGHT | control ID, state |
| 0x13 | TRANSPORT | flags, tempo_hi, tempo_lo, beat, beats_per_bar |
| 0x14 | GRID | cols, rows, cursor_col, cursor_row, cols×rows states (row-major) |
| 0x15 | NOTIFY | title, 0x00, value (each at most 24 characters) |

### TEXT fields

| field | meaning |
|---|---|
| 0x00 | selected track name |
| 0x01 | selected device name (empty = no device) |
| 0x02 | parameter bank name |
| 0x03 | subtitle, for example "Bank 2/5" or "Tracks 1-8" |
| 0x10–0x17 | knob 1–8 label (parameter name, or track name in the mixer view) |
| 0x18–0x1F | knob 1–8 value text, for example "-6.0 dB", "440 Hz", "On" |
| 0x30–0x37 | clip view column headers (track names) |
| 0x38–0x3B | clip view row headers (scene names) |

An empty TEXT clears its field.

### PARAM

- `value` is the normalised position, used for the bar.
- `flags`:
  - bit 0: a parameter is mapped to this knob
  - bit 1: bipolar (draw from the centre)
  - bit 2: stepped / enum

### LIGHT

The host owns the button lights for every control it receives. The device
keeps the lights of the controls it handles itself: Shift, Scale, Arp,
Octave, and the view buttons. For Tempo, the host sets the base state and
the device adds the beat flash on top.

| State | Light |
|---|---|
| 0 | off |
| 1 | dim (available) |
| 2 | on |
| 3 | blink |

### TRANSPORT

- `flags`:
  - bit 0: playing
  - bit 1: arrangement record
  - bit 2: session record
  - bit 3: loop
  - bit 4: metronome
  - bit 5: MIDI capture available
- tempo is BPM × 10 as 14 bits: `tempo_hi << 7 | tempo_lo`.
- `beat`: 0-based beat within the bar, or 127 when stopped.
  - The host sends TRANSPORT on every beat while playing. The device flashes Tempo on each one.

### GRID

The clip view shows 8 columns × 4 rows. The host always sends 8 × 4 and
uses state 0 where a session has no track or scene.

| State | Slot |
|---|---|
| 0 | none (no track or scene) |
| 1 | empty slot |
| 2 | clip, stopped |
| 3 | clip, playing |
| 4 | clip, recording |
| 5 | clip, queued to play |
| 6 | clip, queued to record |
| 7 | empty slot on an armed track |

### NOTIFY

Shows a 1.2 s popup: title (small font), then value (large font). An example
is "Quantize" / "1/16".

## Suggested host behaviour

This is what the Ableton Live script does.

| Control | Device | Volume / Pan | Sends | Clips |
|---|---|---|---|---|
| Knobs | 8 parameters of the selected device bank | volume / pan of 8 tracks | sends A-H of the selected track | volume of the 8 grid tracks |
| Labels (TEXT 0x10+) | parameter names | track names | send names | track names |
| 4D turn | select track | select track | select track | cursor up / down one scene |
| 4D left / right | select track | select track | select track | move cursor (scrolls past the edge) |
| 4D up / down | previous / next device | select track | select track | move cursor |
| 4D push | toggle device on/off | arm selected track | arm selected track | launch clip (Shift: launch scene) |
| Preset ↑ / ↓ | previous / next parameter bank | tracks 1-8 / 9-16 / ... | — | page scenes by 4 |
| Shift + Preset ↑ / ↓ | previous / next device | — | — | — |

| Button | Press | Shift + press |
|---|---|---|
| Play | play / stop | continue |
| Stop | stop | stop all clips |
| Rec | arrangement record | session record |
| Loop | loop | — |
| Metro | metronome | — |
| Tempo | tap tempo | — |
| Undo | undo | redo |
| Quantize | quantize the selected clip | — |
| Ideas | capture MIDI | — |
| Mute | (device: previous knob mode) | mute the selected track |
| Solo | (device: next knob mode) | solo the selected track |

The Ideas light is on while capture is available.
