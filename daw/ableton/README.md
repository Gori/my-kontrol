# My Kontrol for Ableton Live

Ableton Live 12 control surface script for keyboards running the My Kontrol
firmware (first board: Komplete Kontrol M32). The wire protocol is in
[docs/protocol.md](../../docs/protocol.md).

## Install

1. Copy the `MyKontrol` folder to your User Library:
   - macOS: `~/Music/Ableton/User Library/Remote Scripts/`
   - Windows: `\Users\<you>\Documents\Ableton\User Library\Remote Scripts\`
2. Restart Live.
3. Open Preferences > Link, Tempo & MIDI. In a free Control Surface slot pick:
   - Control Surface: **MyKontrol**
   - Input: **My Kontrol DAW**
   - Output: **My Kontrol DAW**

   Live may already have filled this in when the keyboard was plugged in.
4. Under MIDI Ports, turn on **Track** for the **My Kontrol Keys** input.
   That port carries the keys. Leave Track and Remote off for My Kontrol DAW.

On macOS the ports may show up with the device name in front, for example
"My Kontrol M32 My Kontrol DAW". Pick the one that contains "My Kontrol DAW".

If a slot shows "Komplete Kontrol A", "Komplete Kontrol S Mk3" or
"NKS Connect" for this keyboard, set that slot to None. None of Live's
bundled scripts claim this keyboard's USB ID (17CC:1860). NI's own software,
if it is still installed, can create its own "Komplete Kontrol ... DAW" ports.

## Controls

Plug-in, Track and Browser switch between the device, mixer and clip views.

| Control | Device view | Mixer view | Clip view |
|---|---|---|---|
| Knobs | 8 parameters of the selected device | volume of 8 tracks (Shift: pan) | volume of the 8 grid tracks |
| 4D turn | select track | select track | move through scenes |
| 4D left / right | select track | bank tracks by 8 | move cursor |
| 4D up / down | previous / next device | select track | move cursor |
| 4D push | device on/off | arm track | launch clip (Shift: scene) |
| Preset ↑ / ↓ | parameter bank (Shift: device) | | page 4 scenes |

| Button | Press | Shift + press |
|---|---|---|
| Play | play / stop | continue |
| Stop | stop | stop all clips |
| Rec | arrangement record | session record |
| Loop, Metro | loop, metronome | |
| Tempo | tap tempo | |
| Undo | undo | redo |
| Quantize | quantize selected clip | |
| Ideas | capture MIDI | |
| Mute, Solo | selected track | Solo: arm |

Touching a knob starts an automation gesture, so recorded automation is one
clean move.

## Without hardware: the simulator

`sim/mk_sim.py` opens a virtual MIDI port pair called "My Kontrol DAW" and
plays the keyboard's side of the protocol. It prints everything the script
sends and lets you press buttons and turn knobs from the terminal.

```
pip install python-rtmidi
python3 sim/mk_sim.py          # then select "My Kontrol DAW" in Live, type help
```

It also warns when the script sends more than 20 messages or 1 KB within
100 ms. Virtual ports need macOS or Linux. On Windows, create a loopMIDI port
and use `--connect`.

## Development

```
cd daw/ableton/tests
python3 -m unittest            # or: python3 -m pytest
```

The modules that do the work (`protocol`, `outbox`, `session`, `engine`,
the views) never import Live, so they run under plain Python with the fakes
in `tests/fakes.py`. Only `surface.py` and `__init__.py` touch Live.
`test_live_glue.py` loads Live's own `_Framework` from a local Live 12
install with a stubbed `Live` module. It only runs on Python 3.11, because
Live ships Python 3.11 bytecode.

Live writes the script's log to `Log.txt` in its preferences folder; search
for `MyKontrol`.

| File | Role |
|---|---|
| `protocol.py` | bytes on the wire, both directions |
| `outbox.py` | sends only changed fields, at most 20 messages / 960 bytes per 100 ms |
| `session.py` | HI / HELLO / IDENT / PING / BYE |
| `engine.py` | everything above wired together; what the tests drive |
| `controller.py`, `transport.py`, `*_view.py` | what the controls do, what the display shows |
| `surface.py` | the Live `ControlSurface` |

## Protocol notes

The script implements docs/protocol.md v1 with these additions. They are
backward compatible.

- **IDENT has an optional 9th byte: the current view (0–2).** Without it the
  host cannot know which view to send after connecting. If the byte is
  missing, the script keeps its previous view (device view at first).
- **HELLO is accepted at any time.** The script sends HELLO when it loads, so
  a keyboard still in host mode from an earlier script reconnects at once
  instead of after its 3 s timeout.
- **The device owns the Shift, Scale, Arp, Octave and view-button lights.**
  The script never sends LIGHT for them. For Tempo, the host sets the base
  state (dim) and the device flashes it on each beat.
- **Receivers ignore extra trailing data bytes**, so messages can grow later.
- **NOTIFY title and value are at most 24 characters each**, like TEXT.
