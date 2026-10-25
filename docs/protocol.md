# My Kontrol host protocol

Version 2. This is how a DAW talks to a My Kontrol keyboard. Any host can
implement it: a DAW's own control-surface support, a script such as the
Ableton Live script in `daw/ableton/`, or a standalone app. Version 1 hosts
keep working (see [Version 1](#version-1)).

Goals:

- **Semantic, not pixels.** The host sends names, values and states. The
  keyboard decides how to draw them on its own screen and lights. A host
  never formats for a particular screen size.
- **One host implementation serves every keyboard.** Capabilities come from
  the handshake and a machine-readable surface description, never from
  model sniffing.
- **State, not toggles.** The host pushes the state of everything it owns,
  so lights and screens cannot drift after an action the keyboard didn't
  originate.
- **Fits small devices.** The M32 has 9 KB of RAM. Each message is small,
  self-contained and idempotent. Only the surface description and long
  messages come in parts.

Testing without a keyboard: [`firmware/sim`](../firmware/sim/README.md) runs
the real firmware on macOS, with the same MIDI ports as the keyboard.

## Ports

The keyboard is a USB MIDI device with two ports:

| Port | Carries |
|---|---|
| **My Kontrol Keys** | music only: notes, pitch bend, mod, pedals, knob CCs in MIDI mode, MIDI clock |
| **My Kontrol DAW** | control only: this protocol, or Mackie Control. Never notes. |

A host can claim the DAW port exclusively; nothing a user would record ever
arrives on it.

## Framing

- Every message is `F0 7D 4D 4B <cmd> <data...> F7`. There is no running
  status. Data bytes are 7-bit.
- `7D` is the MIDI "non-commercial" manufacturer ID, and other projects
  may use it too. Receivers must match all three bytes `7D 4D 4B` and ignore
  everything else.
- **Maximum length.** The keyboard announces the longest message it accepts
  in `IDENT` (`max_rx`, at least 128 bytes, F0 to F7). Within the field
  budgets below, every message fits in 128 bytes.
- **Unknown messages.** Receivers ignore unknown commands, and ignore trailing
  bytes beyond the ones they understand. This is how messages grow.
- **Numbers.** `u14` is two bytes, high 7 bits first. `pos` is a `u14`
  normalised position: 0 = minimum, 16383 = maximum.
- **Colours.** A colour is three bytes R, G, B, each the 8-bit value shifted
  right by one.
  - `00 00 00` means "no colour".
  - Hosts send black as `01 01 01`.

### Strings

A string is a length byte, then that many encoded bytes.

| Bytes | Meaning |
|---|---|
| 0x20–0x7E | that ASCII character |
| 0x0A | line break; the keyboard may draw it as a space |
| 0x7F hh mm ll | any Unicode scalar value U+0000–U+10FFFF: `hh<<14 \| mm<<7 \| ll` |

Hosts convert from UTF-8.

- **Host side:** each string field has a budget in encoded bytes, given in
  its message table. The host cuts a longer text at the budget, on a
  character boundary.
- **Keyboard side:** it truncates or scrolls whatever its screen cannot
  show. It draws what its fonts have and folds the rest, for example é → e,
  ♯ → #, and anything else to `?`.

## Session

1. **HI.** The keyboard sends `HI`: when USB comes up, and every 2 s while
   no host is connected.
2. **HELLO.** The host answers `HELLO`. It may also send it unprompted, for
   example when it starts.
3. **IDENT.** The keyboard answers `IDENT`, switches into host mode and shows
   "Connected" with the host's name for a moment. In host mode the control
   buttons go to the host instead of acting locally.
4. **Description (optional).** The host sends `DESCRIBE` and reads the
   surface description.
5. **MODES.** The host sends `MODES`, and the keyboard answers with `VIEW`,
   giving the current mode.
6. **Full state.** The host sends its full state for that mode. It sends it
   again after every `VIEW` that changes the mode, and after every
   `REFRESH`. A `VIEW` that repeats the current mode, for example in answer
   to MODES, needs nothing more. Full state includes the "nothing"
   forms, so stale content from before cannot survive:
   - `KEYS_CLEAR` and then any `KEY`s
   - `LIST` 0 when no list is open
   - `MESSAGE` 0 when no message is open
7. **Keepalive.** The host sends at least one message per second; `PING` is
   fine.
8. **BYE.** The host sends `BYE` when it disconnects. `BYE` clears everything
   the host set: slots, page, meters, scale, key labels, lists, messages and
   lights. The keyboard then returns to its own behaviour.

**Lost host.** After 3 s of silence the keyboard marks the host as lost.

- **On screen:** the last state stays, marked "Connection lost", and the
  host's lights go dim. A host can stall for seconds, for example while
  loading a plug-in, and the screen should not blank for that.
- **Recovery:** while the host is lost, the keyboard sends `HI` every 2 s. A
  host that receives `HI` while it believes it is connected must run the
  handshake again and re-send its full state. After 30 s lost, the keyboard
  clears as for `BYE`.

The keyboard also speaks Mackie Control on the same port. It stops sending
`HI` while a Mackie host is connected.

### HI (keyboard → host, 0x02)

`proto` = 1. The keyboard always sends 1 here; a v2 keyboard says so in
`IDENT`.

### HELLO (host → keyboard, 0x00)

| Field | Type | |
|---|---|---|
| proto | byte | 2 (1 for a version 1 host, which sends nothing more) |
| features | u14 | bits listed below |
| name | string ≤ 24 | host name, for example the DAW's product name |
| version | string ≤ 16 | host version |

`features` lists what the host will actually drive. The keyboard keeps its
own behaviour for everything that is missing.

| Bit | Feature |
|---|---|
| 0 | parameters: PAGE / SLOT / VALUE instead of TEXT 0x01–0x03 and 0x10–0x1F |
| 1 | meters |
| 2 | scale |
| 3 | key labels |
| 4 | lists |
| 5 | message screen |
| 6 | colours |
| 7 | clip grid |
| 8 | the 4D encoder: the host handles it as protocol messages |

A host that sets bit 0 never sends the TEXT fields it replaces, and mixing
the two is undefined. TEXT 0x00 (track name) and 0x04 (position) stay in
use.

### IDENT (keyboard → host, 0x01)

| Field | Type | |
|---|---|---|
| proto | byte | 2, or 1 in answer to a version 1 HELLO (which then ends after `view`) |
| board | byte | 1 = Komplete Kontrol M32; other boards get the next numbers (`docs/porting.md`) |
| fw_major, fw_minor, fw_patch | 3 bytes | firmware version |
| keys | byte | number of keys |
| knobs | byte | number of knobs |
| caps | byte | bit 0 touch strips, 1 knob touch sensing, 2 4D encoder, 3 pedal jack; the other bits are 0 (the surface description has the rest) |
| view | byte | the current mode, as in VIEW |
| max_rx | u14 | longest message accepted, F0 to F7 (at least 128) |
| list_rows | byte | list rows the screen shows at once |
| list_cache | byte | list rows the keyboard holds (at least 2 × list_rows) |
| msg_chars | u14 | characters the message screen holds (at least 160) |
| key_labels | byte | key labels the keyboard holds (at least 16) |
| label_chars | byte | characters the keyboard keeps of a key label |
| screen | byte | 0 none, 1 monochrome, 2 colour |
| lights | byte | 0 on/off, 1 levels (LIGHT states), 2 RGB |
| light_guide | byte | number of keys with their own light, 0 if none |

The control layout, knob resolution and screen geometry are in the surface
description.

### DESCRIBE (host → keyboard, 0x07) and DESCRIPTION (keyboard → host, 0x08)

The host sends `DESCRIBE` with no data. The keyboard answers with the
surface description, a JSON document in ASCII, split over messages:

`DESCRIPTION part(u14) parts(u14) json-bytes...`

The host joins the parts in order. A keyboard that doesn't answer within
500 ms has no description. The host then goes on without one, and assumes
the defaults:

- 8 knobs at 100 steps per turn
- the control IDs below

The schema is [`surface.schema.json`](surface.schema.json), and each board's
description is generated at build time from
`firmware/boards/<board>/surface.json`. It lists:

- every control: ID, kind, whether it has a light and what kind, its label
  as printed on the hardware, and whether the keyboard keeps it for itself
- knob count, resolution in steps per turn, and touch sensing
- the strips and the notes their touch is sent on
- the screen: geometry, colour depth, text rows, list rows
- keys: count, lowest note, light guide

A host can map a keyboard it has never seen from this alone.

### PING (0x03), BYE (0x04), REFRESH (keyboard → host, 0x06)

`PING` and `BYE` (host → keyboard) carry no data. The keyboard sends
`REFRESH`, also with no data, when it needs the full state again.

## Controls (keyboard → host)

Everything is on MIDI channel 16.

| Control | Message |
|---|---|
| Button | note-on, note = control ID, velocity 127 = press, 0 = release |
| Knob k (0-based) turned | CC 0x10+k, relative signed-bit: 1..63 = +n, 65..127 = −(v−64) |
| Knob k touched / released | note-on 0x40+k, velocity 127 / 0 |
| Pitch strip / mod strip touched | note-on 0x48 / 0x49, velocity 127 / 0 |
| 4D encoder turned | CC 0x30, relative signed-bit |

- **Shift** is sent like any other button, so the host can build Shift
  combinations.
- **Knob resolution** belongs to the host. It applies its own taper and step
  size, using the steps-per-turn figure from the surface description.
- **The 4D encoder.** By default ("4D as Keys" on Auto) the keyboard types
  the 4D into the focused window as USB keyboard keys: arrows, Return,
  Space, = / -. The host then follows its own selection.
  - A host that sets feature bit 8 gets the 4D as protocol messages instead.
    It should do so only if it gives the 4D something better than the keys
    would, such as selecting tracks without needing focus.
  - The 4D always reaches the host while a list or message is open.
  - The user can override with "4D as Keys": Always (keys even for a host
    with bit 8) or Off (always protocol messages).

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

IDs 32–63 are reserved for controls on other models: 32 Clear, 33 Mixer,
34 Instance, 35 MIDI, 36 Setup, 37 Auto, 38 Count-in, 39 Page ←, 40 Page →,
41 Fixed Vel, 42 Keys, 43 Chord, 44 Back, 45 Enter, 46 Encoder touch, 47 Rewind,
48 Fast forward.

### What the keyboard keeps

In host mode the keyboard still handles these itself:

- Scale and Arp, plus Shift + Scale and Shift + Arp
- Octave − and Octave +
- Shift + Browser, which opens the keyboard's menu
- M / S without Shift, Plug-in and Track: the knob mode (below)
- Browser, when the host offers the clip view

Shift + Mute and Shift + Solo are forwarded as the Mute and Solo notes, with
Shift held. Everything else goes to the host.

## Modes

The keyboard owns the knob mode, so it works the same in every DAW:

- **M / S** step through the modes, in an order the user sets on the
  keyboard.
- **Plug-in** jumps to instrument (Shift: plug-in).
- **Track** jumps to volume (Shift: sends).
- **Browser** toggles the clip view.

The keyboard remembers the mode across restarts.

| id | Mode | The knobs control |
|---|---|---|
| 0 | plug-in | the selected device or plug-in |
| 1 | volume | volume of 8 tracks |
| 2 | clips | the clip grid (GRID) |
| 3 | pan | pan of 8 tracks |
| 4 | sends | sends of the selected track |
| 5 | instrument | the selected track's instrument |
| 16–19 | host-defined | whatever the host names them, for example takes or a browser |

### VIEW (keyboard → host, 0x05)

`id`: the current mode. The keyboard sends it on every mode change, in
answer to every MODES and SET_MODE, and as part of IDENT.

### MODES (host → keyboard, 0x09)

`count, then count × (id, flags, name(string ≤ 16))`

- **Which modes exist.** Only the modes listed exist for this host. A host
  that sends no MODES (version 1) offers the six standard modes.
- **flags.** Bit 0 is enabled. A disabled mode is momentarily meaningless,
  for example plug-in mode with no device selected.
  - The host sends MODES again whenever availability changes.
  - M / S skip disabled modes.
  - If the current mode becomes disabled, the keyboard stays in it. The host
    then sends a PAGE whose title says why, for example "No device", with
    every slot kind 0.
- **name** is the screen title. It may be empty for a standard mode, which
  then uses the keyboard's own title.
- **Order.** M / S follow the user's order for standard modes, with
  host-defined modes after them.
- **Current mode.** If the current mode isn't listed, the keyboard picks the
  first listed one in the user's order.

### SET_MODE (host → keyboard, 0x0A)

`id`

This is advisory, for example to make the knobs follow a plug-in the user
just clicked.

- The keyboard switches if the user's setting *DAW Sets View* is on (the
  default) and the mode is enabled.
- It answers with `VIEW` either way, giving the mode it is actually in.

## Parameters

A page is what the knobs control in the current mode: 8 plug-in parameters,
8 track volumes, and so on. Metadata goes once per page, and value updates
stay tiny.

### PAGE (host → keyboard, 0x20)

| Field | Type | |
|---|---|---|
| gen | byte | 1–127, never 0: it counts up and wraps from 127 to 1, so two pages in a row never share a gen |
| mode | byte | the mode this page belongs to; dropped if it is not the current mode |
| bank | byte | current bank, 0-based |
| banks | byte | number of banks, or 0 if none |
| title | string ≤ 24 | for example a device name, or empty in mixer modes |
| subtitle | string ≤ 24 | for example a bank name, or "Tracks 9-16" |

- **Clearing.** PAGE clears every slot.
- **Stale messages.** Any SLOT, VALUE or slot METER whose `gen` differs from
  the current page's is dropped, so late messages from the previous page
  never land on the new knobs.
- **Mode changes.** A mode change drops the page too, until the host sends a
  new one.

### SLOT (host → keyboard, 0x21)

One per knob, after PAGE.

| Field | Type | |
|---|---|---|
| gen | byte | the page's gen |
| knob | byte | 0-based |
| kind | byte | 0 empty, 1 continuous, 2 toggle, 3 enum |
| flags | byte | bit 0 bipolar, bit 1 has default |
| steps | byte | number of enum items; 0 for continuous and toggle |
| default | pos | |
| colour | colour | for example the track colour in mixer modes |
| name | string ≤ 24 | parameter or track name |
| group | string ≤ 16 | for example "Filter" or "Envelope 2", or empty |

A host that cannot tell an enum from a continuous parameter sends kind 1.
The keyboard draws a bar and the value text either way.

A SLOT may be sent again within the same page, for example after a rename.
It replaces that knob's metadata and keeps its value.

### VALUE (host → keyboard, 0x22)

`gen, knob, pos(u14), text(string ≤ 24)`

- **text** is the host's own value text with its unit: "-6.0 dB", "250 ms",
  "Hall", "+7 st".
- **Empty text.** A length of 0 means the text is unchanged. While a knob is
  moving, the host can send positions only, at full rate, and send the text
  when it changes or settles.
- **Same pos, new text.** A VALUE with an unchanged `pos` and a non-empty
  text is a legal text-only update, for example when a plug-in's own value
  text arrives late. The keyboard never drops a VALUE because its `pos` is
  unchanged.
- **When to send.** The host sends VALUE whenever the value changes, whoever
  changed it.

## Meters

### METER (host → keyboard, 0x23)

`target, kind, gen, count, count × level`

| target | levels |
|---|---|
| 0 | the page's slots 1..count; `gen` must match the page |
| 1 | the selected channel, count 2 = left / right |
| 2 | the master, count 2 = left / right |

`gen` is ignored for targets 1 and 2.

| kind | level byte |
|---|---|
| 0 signal | 0 = silence, 1..127 linear in dB from −60 dBFS to 0 dBFS and above |
| 1 gain reduction | 0..127 linear from 0 dB to −24 dB |

- **Kinds.** A host sends only the kinds it has. Gain reduction is optional.
- **Rate.** The host sends one message per frame per target, at most 20
  per second. It may skip a frame identical to the last one, but sends at
  least one frame every 500 ms while any level is above 0. The keyboard may
  draw less often.
- **Expiry.** A target's levels drop to 0 one second after its last frame,
  and on every mode change for target 1. A host that stops metering needs
  to send nothing.

## Text and notifications

### TEXT (host → keyboard, 0x10)

`field, chars...` (the rest of the message, ASCII, at most 24 characters;
empty clears the field)

| field | meaning |
|---|---|
| 0x00 | selected track name |
| 0x01 | selected device name (empty = no device) — version 1 hosts |
| 0x02 | parameter bank name — version 1 hosts |
| 0x03 | subtitle, for example "Bank 2/5" — version 1 hosts |
| 0x04 | position as the host shows it, for example "12.3.1" or "00:01:23" |
| 0x10–0x17 | knob 1–8 label — version 1 hosts |
| 0x18–0x1F | knob 1–8 value text — version 1 hosts |
| 0x30–0x37 | clip view column headers (track names) |
| 0x38–0x3B | clip view row headers (scene names) |

### NOTIFY (host → keyboard, 0x15)

`title, 0x00, value` (ASCII, each at most 24 characters)

This shows a 1.2 s popup: the title in the small font, then the value in
the large font, for example "Quantize" / "1/16".

## Transport

### TRANSPORT (host → keyboard, 0x13)

`flags, tempo_hi, tempo_lo, beat, beats_per_bar, flags2`

| flags bit | | flags2 bit | |
|---|---|---|---|
| 0 | playing | 0 | punch in/out active |
| 1 | arrangement record | 1 | count-in active |
| 2 | session record | 2 | follow on |
| 3 | loop | | |
| 4 | metronome | | |
| 5 | MIDI capture available | | |

- **tempo** is BPM × 10 as 14 bits: `tempo_hi << 7 | tempo_lo`.
- **beat** is the 0-based beat within the bar, or 127 when stopped. The host
  sends TRANSPORT on every beat while playing, and the keyboard flashes
  Tempo on each one.
- **When to send.** The host sends TRANSPORT whenever any field changes. It
  never relies on the keyboard having changed it.

## Lights

### LIGHT (host → keyboard, 0x12) and LIGHT_RGB (0x29)

`control, state` and `control, state, colour`

| State | Light |
|---|---|
| 0 | off |
| 1 | dim (available) |
| 2 | on |
| 3 | blink |

- **Ownership.** The host owns the lights of every control it receives. The
  keyboard keeps the lights of the controls it handles itself.
- **Tempo.** The host sets the base state, and the keyboard adds the beat
  flash on top.
- **Colour.** Keyboards without RGB lights use only `state`.

## Clip grid

### GRID (host → keyboard, 0x14)

`cols, rows, cursor_col, cursor_row, cols × rows states (row-major)`

The clip view shows 8 columns × 4 rows. The host always sends 8 × 4, and
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

## Scale and keys

### SCALE (host → keyboard, 0x24)

`state, root, mask(u14), name(string ≤ 24, optional)`

- **state**
  - 0: no key is set. This is distinct from C major: the keyboard lights
    every key, and Smart Play keeps the user's own scale.
  - 1: the key is set by the song.
  - 2: the key is set by the clip being edited. Hosts without per-clip keys
    send 1.
- **root** is 0–11, with 0 = C.
- **mask**: bit n is set when the note root+n is in the scale; bit 0 is
  always set.
- **name** is the host's name for the scale, for example "Dorian". It is
  shown when the keyboard doesn't recognise the mask.
- **On the keyboard:**
  - With *DAW Scale* set to Follow (the default), Smart Play's scale uses
    the DAW's key whenever one is set.
  - Keyboards with a light guide show it.

### KEY (host → keyboard, 0x25)

`note, role, colour, label(string ≤ 16)`

This names one key, for example a drum lane.

- **Clearing.** A KEY with role 0 and an empty label clears that note.
- **Updates.** A KEY for a note already held replaces it.
- **Over the limit.** The keyboard keeps the first `key_labels` keys after
  KEYS_CLEAR and drops the rest. Hosts send in priority order, for example
  kick, snare and hats first.
- **role** is one of:
  - 0 none
  - 1 kick, 2 snare, 3 rim, 4 clap
  - 5 closed hat, 6 pedal hat, 7 open hat
  - 8 tom, 9 cymbal, 10 percussion, 11 other
- **On the keyboard:**
  - Without a light guide, it shows the label of the key just played.
  - With a light guide, the key lights in its colour, or else in the role's
    colour.

### KEYS_CLEAR (host → keyboard, 0x2A)

No data. Clears every key label.

## Lists

A list is a modal screen the host drives: a browser, takes, generated
candidates, presets. The host owns the cursor.

### LIST (host → keyboard, 0x26)

`id, total(u14), cursor(u14), first(u14), title(string ≤ 24)`

- **id.** `id` 1–127 identifies the list, and `id` 0 closes it. A new `id`
  drops every row held.
- **cursor and first.** `cursor` is the highlighted item, and `first` is the
  item in the top visible row.

### ROW (host → keyboard, 0x27)

`id, index(u14), flags, text(string ≤ 40)`

- **index** is the item's position in the list, not on screen.
- **Overscan.** The host may send rows outside the visible window, up to
  `list_cache` in all. A cursor step then redraws from rows the keyboard
  already has, while the host fills in ahead.
- **Eviction.** When the cache is full, the keyboard drops the row farthest
  from the cursor. Of two equally far rows it drops the one with the higher
  index. If that row is the new one, the keyboard drops the new row. The rule
  is deterministic, so a host may model the cache to know which rows to send.
  A redundant ROW is harmless.
- **flags:**
  - bit 0 folder
  - bit 1 playing/previewing
  - bit 2 checked
  - bit 3 disabled

While a list is open, the 4D encoder goes to the host as protocol messages,
even with "4D as Keys" on:

- turn is CC 0x30
- push is note 25
- left is note 22, which the host uses as "back". At the top level, the host
  may close the list or do nothing.

The host moves the cursor and sends LIST again, with ROW for any rows the
keyboard doesn't hold. The keyboard's own menu (Shift + Browser) still works
over a list.

## Message screen

### MESSAGE (host → keyboard, 0x28)

`id, part, flags, text(string ≤ 64)`

- **id.** `id` 1–127 identifies the message, and `id` 0 closes it.
- **parts.** Longer text arrives in parts with the same `id`. Part 0
  replaces the text, and later parts are appended, up to `msg_chars`.
- **flags.**
  - bit 0: last part
  - bit 1: busy, shown as an activity mark, for example while the host is
    thinking or listening
- **On screen.** The keyboard wraps the words, and 0x0A breaks a line. A
  message is drawn over a list.
- **4D encoder.** Turning it scrolls the text on the keyboard. Push (note
  25) and left (note 22) go to the host, for confirm and back.

Lists and messages are generic. With a button held for speaking, MESSAGE
for the reply, and LIST for candidates to choose from, they make a complete
voice or assistant front-end. They need nothing host-specific in the
keyboard.

## Command summary

| cmd | Name | Direction |
|---|---|---|
| 0x00 | HELLO | host → keyboard |
| 0x01 | IDENT | keyboard → host |
| 0x02 | HI | keyboard → host |
| 0x03 | PING | host → keyboard |
| 0x04 | BYE | host → keyboard |
| 0x05 | VIEW | keyboard → host |
| 0x06 | REFRESH | keyboard → host |
| 0x07 | DESCRIBE | host → keyboard |
| 0x08 | DESCRIPTION | keyboard → host |
| 0x09 | MODES | host → keyboard |
| 0x0A | SET_MODE | host → keyboard |
| 0x10 | TEXT | host → keyboard |
| 0x11 | PARAM (version 1) | host → keyboard |
| 0x12 | LIGHT | host → keyboard |
| 0x13 | TRANSPORT | host → keyboard |
| 0x14 | GRID | host → keyboard |
| 0x15 | NOTIFY | host → keyboard |
| 0x20 | PAGE | host → keyboard |
| 0x21 | SLOT | host → keyboard |
| 0x22 | VALUE | host → keyboard |
| 0x23 | METER | host → keyboard |
| 0x24 | SCALE | host → keyboard |
| 0x25 | KEY | host → keyboard |
| 0x26 | LIST | host → keyboard |
| 0x27 | ROW | host → keyboard |
| 0x28 | MESSAGE | host → keyboard |
| 0x29 | LIGHT_RGB | host → keyboard |
| 0x2A | KEYS_CLEAR | host → keyboard |

## Suggested host behaviour

This is what the Ableton Live script does, and what a host should do unless
it has a better idea.

| Control | Instrument (5) | Plug-in (0) | Volume (1) / Pan (3) | Sends (4) | Clips (2) |
|---|---|---|---|---|---|
| Knobs | the selected track's instrument; a Rack's macros first | the selected device | volume / pan of 8 tracks | sends of the selected track | volume of the 8 grid tracks |
| PAGE title / subtitle | device / bank name | device / bank name | — / "Tracks 9-16" | — / "Sends A-D" | — |
| Preset ↑ / ↓ | previous / next bank | previous / next bank | tracks by 8 | — | page scenes by 4 |
| Shift + Preset ↑ / ↓ | — | previous / next device | — | — | — |

| Button | Press | Shift + press |
|---|---|---|
| Play | play | back to start |
| Stop | stop; when stopped, back to start | stop all clips |
| Rec | record | session record |
| Loop | loop | — |
| Metro | metronome | — |
| Tempo | tap tempo | — |
| Undo | undo | redo |
| Quantize | quantize the selection | — |
| Ideas | capture MIDI | — |
| Mute / Solo | (keyboard: knob mode) | mute / solo the selected track |

More suggestions:

- **Knob steps.**
  - A stepped parameter moves one item per few knob steps, with the
    sub-steps accumulating.
  - A continuous one moves about 1/200 of its range per step.
- **Value text.** The knob being turned or touched jumps the queue, so its
  value text follows the turn without delay.
- **Shift + touching a knob** resets its parameter to its default.
- **Touching a knob** may also highlight the parameter in the host, for
  example its automation lane.
- **Selecting a device with the mouse** may send `SET_MODE` with the plug-in
  or instrument mode, so the knobs follow it.
- **Preset ↑ / ↓** page banks in parameter modes. In a host-defined takes
  mode they step through the selected clip's takes.
- **Send state after every change**, including changes made with the mouse.
  Diff on the host side, and don't send unchanged values.
- **Lights.**
  - Play is on while playing.
  - Rec is on while recording.
  - Loop and Metro show their state.
  - Undo, Quantize, Stop, Tempo, Mute and Solo are dim when available.

## Version 1

A version 1 host sends `HELLO` with proto 1 and gets a 9-byte IDENT. It uses:

- TEXT 0x01–0x03 and 0x10–0x1F
- PARAM (0x11): `knob, value 0–127, flags`, where flags are bit 0 mapped,
  bit 1 bipolar and bit 2 stepped
- LIGHT, TRANSPORT (without flags2), GRID and NOTIFY

It sends no MODES, so the six standard modes are offered. On loss it is
marked lost and recovers as above. Everything else in this document is
version 2 only.
