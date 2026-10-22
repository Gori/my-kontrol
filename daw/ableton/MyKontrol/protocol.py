# My Kontrol host protocol v1: constants, encoders and decoders.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This module is the single source of truth for the bytes on the wire. It is
# plain Python with no dependency on Live, so it is shared by the remote
# script, the unit tests and the device simulator (sim/mk_sim.py).
#
# Spec: docs/protocol.md. Both directions are implemented here: the host side
# (what the script sends / parses) and the device side (used by the simulator
# and the tests to play the keyboard's role).

from collections import namedtuple

try:
    import unicodedata
except ImportError:   # not every embedded Python has it; degrade to '?'
    unicodedata = None

PROTOCOL_VERSION = 1

# Sysex framing: F0 7D 4D 4B <cmd> <data...> F7
SYSEX_START = 0xF0
SYSEX_END = 0xF7
MANUFACTURER_ID = 0x7D  # non-commercial / educational use
HEADER = (SYSEX_START, MANUFACTURER_ID, 0x4D, 0x4B)  # "MK"

# Device -> host commands
CMD_IDENT = 0x01
CMD_HI = 0x02
CMD_VIEW = 0x05
CMD_REFRESH = 0x06

# Host -> device commands
CMD_HELLO = 0x00
CMD_PING = 0x03
CMD_BYE = 0x04
CMD_TEXT = 0x10
CMD_PARAM = 0x11
CMD_LIGHT = 0x12
CMD_TRANSPORT = 0x13
CMD_GRID = 0x14
CMD_NOTIFY = 0x15

# Views, as carried by VIEW
VIEW_PLUGIN = 0          # the selected device
VIEW_VOLUME = 1
VIEW_CLIPS = 2
VIEW_PAN = 3
VIEW_SENDS = 4
VIEW_INSTRUMENT = 5      # the selected track's instrument
VIEWS = (VIEW_PLUGIN, VIEW_VOLUME, VIEW_CLIPS, VIEW_PAN, VIEW_SENDS, VIEW_INSTRUMENT)
VIEW_NAMES = {VIEW_PLUGIN: "plug-in", VIEW_VOLUME: "volume", VIEW_CLIPS: "clips",
              VIEW_PAN: "pan", VIEW_SENDS: "sends", VIEW_INSTRUMENT: "instrument"}

# Boards, as carried by IDENT
BOARD_NAMES = {1: "Komplete Kontrol M32"}

# IDENT caps bits
CAP_TOUCH_STRIPS = 1 << 0
CAP_KNOB_TOUCH = 1 << 1
CAP_4D_ENCODER = 1 << 2
CAP_PEDAL_JACK = 1 << 3

# All controls travel on MIDI channel 16 (0-based 15).
CONTROL_CHANNEL = 15
NOTE_ON = 0x90 | CONTROL_CHANNEL
NOTE_OFF = 0x80 | CONTROL_CHANNEL
CONTROL_CHANGE = 0xB0 | CONTROL_CHANNEL

KNOB_CC_BASE = 0x10       # CC 0x10 + k: knob k turned
KNOB_CC_LAST = 0x2F       # room for 32 knobs before the 4D encoder CC
ENCODER_CC = 0x30         # 4D encoder turned
KNOB_TOUCH_BASE = 0x40    # note 0x40 + k: knob k touched
BUTTON_LAST = 0x3F        # control IDs 0..63 are buttons

# Control IDs (shared by every keyboard; firmware/core/controls.h)
CTL_SHIFT = 0
CTL_SCALE = 1
CTL_ARP = 2
CTL_UNDO = 3
CTL_QUANTIZE = 4
CTL_IDEAS = 5
CTL_LOOP = 6
CTL_METRO = 7
CTL_TEMPO = 8
CTL_PLAY = 9
CTL_REC = 10
CTL_STOP = 11
CTL_PRESET_UP = 12
CTL_PRESET_DOWN = 13
CTL_MUTE = 14
CTL_SOLO = 15
CTL_BROWSER = 16
CTL_PLUGIN = 17
CTL_TRACK = 18
CTL_OCT_DOWN = 19
CTL_OCT_UP = 20
CTL_NAV_UP = 21
CTL_NAV_LEFT = 22
CTL_NAV_RIGHT = 23
CTL_NAV_DOWN = 24
CTL_NAV_PUSH = 25
CTL_PEDAL_PLUG = 26
# 32..63: controls on other models
CTL_CLEAR = 32
CTL_MIXER = 33
CTL_INSTANCE = 34
CTL_MIDI = 35
CTL_SETUP = 36
CTL_AUTO = 37
CTL_COUNT_IN = 38
CTL_PAGE_LEFT = 39
CTL_PAGE_RIGHT = 40
CTL_FIXED_VEL = 41

CONTROL_NAMES = {
    CTL_SHIFT: "shift", CTL_SCALE: "scale", CTL_ARP: "arp", CTL_UNDO: "undo",
    CTL_QUANTIZE: "quantize", CTL_IDEAS: "ideas", CTL_LOOP: "loop",
    CTL_METRO: "metro", CTL_TEMPO: "tempo", CTL_PLAY: "play", CTL_REC: "rec",
    CTL_STOP: "stop", CTL_PRESET_UP: "preset_up", CTL_PRESET_DOWN: "preset_down",
    CTL_MUTE: "mute", CTL_SOLO: "solo", CTL_BROWSER: "browser",
    CTL_PLUGIN: "plugin", CTL_TRACK: "track", CTL_OCT_DOWN: "oct_down",
    CTL_OCT_UP: "oct_up", CTL_NAV_UP: "up", CTL_NAV_LEFT: "left",
    CTL_NAV_RIGHT: "right", CTL_NAV_DOWN: "down", CTL_NAV_PUSH: "push",
    CTL_PEDAL_PLUG: "pedal", CTL_CLEAR: "clear", CTL_MIXER: "mixer",
    CTL_INSTANCE: "instance", CTL_MIDI: "midi", CTL_SETUP: "setup",
    CTL_AUTO: "auto", CTL_COUNT_IN: "count_in", CTL_PAGE_LEFT: "page_left",
    CTL_PAGE_RIGHT: "page_right", CTL_FIXED_VEL: "fixed_vel",
}

# Controls the device keeps for itself, even in host mode. The host never
# sends LIGHT for these (see the spec-gap notes in README.md).
DEVICE_OWNED_CONTROLS = frozenset((
    CTL_SHIFT, CTL_SCALE, CTL_ARP, CTL_OCT_DOWN, CTL_OCT_UP,
    CTL_BROWSER, CTL_PLUGIN, CTL_TRACK,
))

# TEXT fields
TEXT_TRACK = 0x00
TEXT_DEVICE = 0x01
TEXT_BANK = 0x02
TEXT_SUBTITLE = 0x03
TEXT_KNOB_LABEL = 0x10   # + knob (0..7)
TEXT_KNOB_VALUE = 0x18   # + knob (0..7)
TEXT_COLUMN = 0x30       # + column (0..7): clip view track names
TEXT_ROW = 0x38          # + row (0..3): clip view scene names
MAX_TEXT = 24

# PARAM flags
PARAM_MAPPED = 1 << 0
PARAM_BIPOLAR = 1 << 1
PARAM_STEPPED = 1 << 2

# LIGHT states
LIGHT_OFF = 0
LIGHT_DIM = 1
LIGHT_ON = 2
LIGHT_BLINK = 3

# TRANSPORT flags
TRANSPORT_PLAYING = 1 << 0
TRANSPORT_ARRANGEMENT_REC = 1 << 1
TRANSPORT_SESSION_REC = 1 << 2
TRANSPORT_LOOP = 1 << 3
TRANSPORT_METRONOME = 1 << 4
TRANSPORT_CAPTURE = 1 << 5
BEAT_STOPPED = 127
MAX_TEMPO_X10 = (1 << 14) - 1

# GRID
GRID_COLS = 8
GRID_ROWS = 4
SLOT_NONE = 0
SLOT_EMPTY = 1
SLOT_STOPPED = 2
SLOT_PLAYING = 3
SLOT_RECORDING = 4
SLOT_QUEUED = 5
SLOT_QUEUED_RECORD = 6
SLOT_ARMED = 7

# ---------------------------------------------------------------------------
# Events. parse() returns these for device -> host traffic, parse_host() for
# host -> device traffic. Unknown or malformed input returns None.
# ---------------------------------------------------------------------------

Hi = namedtuple("Hi", "proto")
# view: the device's current view (9th IDENT byte); None if older firmware
# left it out.
Ident = namedtuple("Ident", "proto board fw keys knobs caps view")
View = namedtuple("View", "view")
Refresh = namedtuple("Refresh", "")
Button = namedtuple("Button", "control pressed")
KnobTurn = namedtuple("KnobTurn", "knob delta")
KnobTouch = namedtuple("KnobTouch", "knob touched")
EncoderTurn = namedtuple("EncoderTurn", "delta")

Hello = namedtuple("Hello", "proto")
Ping = namedtuple("Ping", "")
Bye = namedtuple("Bye", "")
Text = namedtuple("Text", "field text")
Param = namedtuple("Param", "knob value flags")
Light = namedtuple("Light", "control state")
Transport = namedtuple("Transport", "flags tempo beat beats_per_bar")
Grid = namedtuple("Grid", "cols rows cursor_col cursor_row states")
Notify = namedtuple("Notify", "title value")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Characters NFKD cannot reduce to ASCII but that show up in Live's names and
# value strings.
_ASCII_FALLBACK = {
    "−": "-", "–": "-", "—": "-", "‐": "-",
    "‘": "'", "’": "'", "“": '"', "”": '"',
    "…": "...", "∞": "inf", "°": "deg", "µ": "u",
    "μ": "u", "ß": "ss", "æ": "ae", "Æ": "AE",
    "ø": "o", "Ø": "O", "ł": "l", "Ł": "L",
    "×": "x", "±": "+/-", "♯": "#", "♭": "b",
}


def to_ascii(value, limit=MAX_TEXT):
    """Return value as printable ASCII (0x20..0x7E), at most limit chars.

    Accented letters lose their accents, a few common symbols get ASCII
    spellings and anything else becomes '?'. Whitespace becomes a space.
    """
    if value is None:
        return ""
    out = []
    for ch in str(value):
        code = ord(ch)
        if 0x20 <= code <= 0x7E:
            out.append(ch)
        elif ch.isspace():
            out.append(" ")
        elif ch in _ASCII_FALLBACK:
            out.append(_ASCII_FALLBACK[ch])
        elif unicodedata is not None:
            base = "".join(c for c in unicodedata.normalize("NFKD", ch)
                           if 0x20 <= ord(c) <= 0x7E)
            out.append(base or "?")
        else:
            out.append("?")
    text = "".join(out)
    return text[:limit] if limit is not None else text


def _data(value):
    """Clamp an int to a 7-bit data byte."""
    return max(0, min(0x7F, int(value)))


def _chars(text, limit=MAX_TEXT):
    return [ord(c) for c in to_ascii(text, limit)]


def _sysex(cmd, data=()):
    return HEADER + (cmd,) + tuple(_data(b) for b in data) + (SYSEX_END,)


def decode_relative(value):
    """Decode a relative signed-bit CC value: 1..63 = +n, 65..127 = -(v-64)."""
    value &= 0x7F
    if value < 64:
        return value
    return -(value - 64)


def encode_relative(delta):
    """Encode a delta as relative signed-bit (clamped to +-63)."""
    delta = int(delta)
    if delta >= 0:
        return min(delta, 63)
    return 64 + min(-delta, 63)


def tempo_bytes(bpm):
    """BPM -> (tempo_hi, tempo_lo): BPM x 10 as 14 bits."""
    x10 = max(0, min(MAX_TEMPO_X10, int(round(float(bpm) * 10))))
    return x10 >> 7, x10 & 0x7F


def normalised(fraction):
    """0.0..1.0 -> 0..127 (rounded, clamped)."""
    return _data(round(max(0.0, min(1.0, float(fraction))) * 127))


# ---------------------------------------------------------------------------
# Host -> device encoders (used by the script)
# ---------------------------------------------------------------------------

def hello():
    return _sysex(CMD_HELLO, (PROTOCOL_VERSION,))


def ping():
    return _sysex(CMD_PING)


def bye():
    return _sysex(CMD_BYE)


def text(field, value):
    return _sysex(CMD_TEXT, [field] + _chars(value))


def param(knob, value, flags):
    return _sysex(CMD_PARAM, (knob, value, flags & 0x07))


def light(control, state):
    return _sysex(CMD_LIGHT, (control, state & 0x03))


def transport(flags, bpm, beat, beats_per_bar):
    hi, lo = tempo_bytes(bpm)
    return _sysex(CMD_TRANSPORT, (flags & 0x3F, hi, lo, beat, beats_per_bar))


def grid(cols, rows, cursor_col, cursor_row, states):
    states = list(states)
    if len(states) != cols * rows:
        raise ValueError("GRID needs cols x rows states")
    return _sysex(CMD_GRID, [cols, rows, cursor_col, cursor_row] + states)


def notify(title, value):
    data = _chars(title) + [0x00] + _chars(value)
    return _sysex(CMD_NOTIFY, data)


# ---------------------------------------------------------------------------
# Device -> host encoders (used by the simulator and the tests)
# ---------------------------------------------------------------------------

def hi():
    return _sysex(CMD_HI, (PROTOCOL_VERSION,))


def ident(board=1, fw=(0, 1, 0), keys=32, knobs=8, caps=0, view=None):
    data = (PROTOCOL_VERSION, board) + tuple(fw) + (keys, knobs, caps)
    if view is not None:
        data += (view,)
    return _sysex(CMD_IDENT, data)


def view(view_id):
    return _sysex(CMD_VIEW, (view_id,))


def refresh():
    return _sysex(CMD_REFRESH)


def button(control, pressed):
    return (NOTE_ON, _data(control), 127 if pressed else 0)


def knob_turn(knob, delta):
    return (CONTROL_CHANGE, KNOB_CC_BASE + knob, encode_relative(delta))


def knob_touch(knob, touched):
    return (NOTE_ON, KNOB_TOUCH_BASE + knob, 127 if touched else 0)


def encoder_turn(delta):
    return (CONTROL_CHANGE, ENCODER_CC, encode_relative(delta))


# ---------------------------------------------------------------------------
# Decoders
# ---------------------------------------------------------------------------

def _split_sysex(midi_bytes):
    """Return (cmd, data tuple) for a My Kontrol sysex, else None."""
    b = tuple(midi_bytes)
    if len(b) < len(HEADER) + 2 or b[:len(HEADER)] != HEADER or b[-1] != SYSEX_END:
        return None
    return b[len(HEADER)], b[len(HEADER) + 1:-1]


def _string(data):
    return "".join(chr(c) for c in data if 0x20 <= c <= 0x7E)


def parse(midi_bytes):
    """Decode one device -> host message. Returns an event or None.

    Trailing data bytes beyond what v1 defines are ignored, so newer
    firmware can extend a message without breaking this script.
    """
    b = tuple(midi_bytes)
    if not b:
        return None
    if b[0] == SYSEX_START:
        split = _split_sysex(b)
        if split is None:
            return None
        cmd, data = split
        if cmd == CMD_HI and len(data) >= 1:
            return Hi(data[0])
        if cmd == CMD_IDENT and len(data) >= 8:
            view_id = data[8] if len(data) >= 9 and data[8] in VIEWS else None
            return Ident(data[0], data[1], tuple(data[2:5]), data[5], data[6], data[7],
                         view_id)
        if cmd == CMD_VIEW and len(data) >= 1:
            return View(data[0])
        if cmd == CMD_REFRESH:
            return Refresh()
        return None
    if len(b) != 3:
        return None
    status, num, value = b
    if status == NOTE_ON or status == NOTE_OFF:
        pressed = status == NOTE_ON and value > 0
        if num <= BUTTON_LAST:
            return Button(num, pressed)
        return KnobTouch(num - KNOB_TOUCH_BASE, pressed)
    if status == CONTROL_CHANGE:
        if KNOB_CC_BASE <= num <= KNOB_CC_LAST:
            return KnobTurn(num - KNOB_CC_BASE, decode_relative(value))
        if num == ENCODER_CC:
            return EncoderTurn(decode_relative(value))
    return None


def parse_host(midi_bytes):
    """Decode one host -> device message. Returns an event or None."""
    split = _split_sysex(midi_bytes)
    if split is None:
        return None
    cmd, data = split
    if cmd == CMD_HELLO and len(data) >= 1:
        return Hello(data[0])
    if cmd == CMD_PING:
        return Ping()
    if cmd == CMD_BYE:
        return Bye()
    if cmd == CMD_TEXT and len(data) >= 1:
        return Text(data[0], _string(data[1:]))
    if cmd == CMD_PARAM and len(data) >= 3:
        return Param(data[0], data[1], data[2])
    if cmd == CMD_LIGHT and len(data) >= 2:
        return Light(data[0], data[1])
    if cmd == CMD_TRANSPORT and len(data) >= 5:
        return Transport(data[0], ((data[1] << 7) | data[2]) / 10.0, data[3], data[4])
    if cmd == CMD_GRID and len(data) >= 4:
        cols, rows = data[0], data[1]
        states = data[4:]
        if len(states) != cols * rows:
            return None
        return Grid(cols, rows, data[2], data[3], tuple(states))
    if cmd == CMD_NOTIFY:
        if 0 in data:
            sep = data.index(0)
            return Notify(_string(data[:sep]), _string(data[sep + 1:]))
        return Notify(_string(data), "")
    return None
