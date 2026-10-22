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

The knobs have five modes plus the clip view. M and S step through the modes
(default order: Instrument, Plug-in, Volume, Pan, Sends). The keyboard also
has direct buttons for them:

| Button | Mode | With Shift |
|---|---|---|
| Plug-in | Instrument | Plug-in |
| Track | Volume | Sends |
| Browser | Clips | keyboard menu |

| Mode | Knobs | Preset ↑ / ↓ | Shift + Preset ↑ / ↓ |
|---|---|---|---|
| Instrument | the selected track's instrument. A Rack's 8 macros come first, then the instrument inside it | bank | |
| Plug-in | the device selected in Live | bank | previous / next device |
| Volume, Pan | 8 tracks | tracks 1-8, 9-16, ... | |
| Sends | sends A-H of the selected track | | |
| Clips | volume of the 8 grid tracks | page scenes by 4 | |

Built-in devices (Operator, Wavetable, Drift, Simpler, Analog...) get Live's
own banks, with names like "Oscillators" or "Filter Envelopes". Plug-ins get
their parameters 8 at a time. The screen shows the device, bank name and
position ("2/6"). Touch a knob to see its full name and value.

| Button | Press | Shift + press |
|---|---|---|
| Play | play | back to start |
| Stop | stop; when stopped, back to start | stop all clips |
| Rec | arrangement record | session record |
| Loop, Metro | loop, metronome | |
| Tempo | tap tempo | |
| Undo | undo | redo |
| Quantize | quantize the selected clip | |
| Ideas | capture MIDI | |
| M, S | knob mode − / + | mute / solo the selected track |

**4D encoder.** By default the keyboard types it into Live as keys: arrows,
Return, Space ("4D as Keys" setting). It moves Live's own selection, and
the screen and the clip-view cursor follow. With "4D as Keys" off it works
like this:

| 4D | Instrument | Plug-in | Volume / Pan / Sends | Clips |
|---|---|---|---|---|
| Turn, left / right | select track | select track | select track | move cursor |
| Up / down | bank | previous / next device | select track | move cursor |
| Push | instrument on/off | device on/off | arm track | launch clip (Shift: scene) |

Touching a knob also starts an automation gesture, so recorded automation is
one clean move.

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
| `banks.py` | parameter banks from Live's bank definitions |
| `controller.py`, `transport.py`, `*_view.py` (device = instrument/plug-in, mixer = volume/pan, sends, clip, browser) | what the controls do, what the display shows |
| `surface.py` | the Live `ControlSurface` |

## Protocol notes

The script speaks protocol version 2 and falls back to version 1. It always
sends a v2 HELLO. Firmware that only
knows v1 answers with a v1 IDENT, and the script then behaves exactly as
before. If a v1 IDENT arrives without the view byte, the script keeps its
previous view (instrument view at first).

With a v2 keyboard:

| Feature | What Live fills in |
|---|---|
| MODES | instrument, plug-in, volume, pan, sends, clips, and host mode 16 "Browser". Instrument / plug-in are disabled on a track without an instrument / device, sends without return tracks |
| SET_MODE | clicking another device on the selected track in Live suggests plug-in mode, or instrument mode for the track's instrument |
| PAGE / SLOT / VALUE | replace TEXT 0x01–0x03, 0x10–0x1F and PARAM. Toggle and enum slots for switches and choice parameters, Live's defaults, track colours in the mixer modes. While a knob turns, VALUE carries the text only when Live's text changes |
| METER | track output in volume / pan / sends (one level per knob), the selected track L / R in instrument / plug-in mode. 10 per second |
| TRANSPORT | punch, count-in and follow flags; TEXT 0x04 is the position, "bar.beat.sixteenth" |
| SCALE | Live 12's song key while Scale Mode is on; no key otherwise and in older Live |
| KEY | pad names of a Drum Rack on the selected track, kick, snare and hats first |
| LIST | browser mode: 4D turn / up / down move, push loads (or opens a folder), right opens, left goes back, Preset ↑ / ↓ page |

Not used: DESCRIBE (the script assumes 8 knobs and its own knob feel),
LIGHT_RGB (Live has no button colours to show) and the message screen
(nothing in Live needs it; the script only sends MESSAGE 0).

Live does not document its meter scale; the conversion to the METER dB
scale is an approximation, described in `liveutil.py`.

`sim/mk_sim.py` plays a v2 keyboard by default; `--proto 1` plays older
firmware.
