# My Kontrol host protocol (v1 and v2): constants, encoders and decoders.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This module is the single source of truth for the bytes on the wire. It is
# plain Python with no dependency on Live, so it is shared by the remote
# script, the unit tests and the device simulator (sim/mk_sim.py).
#
# Spec: docs/protocol.md (version 2, which extends version 1).
# Both directions are implemented here: the host side (what the script sends
# / parses) and the device side (used by the simulator and the tests to play
# the keyboard's role).

from collections import namedtuple

try:
    import unicodedata
except ImportError:   # not every embedded Python has it; degrade to '?'
    unicodedata = None

PROTOCOL_VERSION = 1     # HI and v1 HELLO / IDENT
PROTOCOL_V2 = 2

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
CMD_DESCRIPTION = 0x08   # v2

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
# v2
CMD_DESCRIBE = 0x07
CMD_MODES = 0x09
CMD_SET_MODE = 0x0A
CMD_PAGE = 0x20
CMD_SLOT = 0x21
CMD_VALUE = 0x22
CMD_METER = 0x23
CMD_SCALE = 0x24
CMD_KEY = 0x25
CMD_LIST = 0x26
CMD_ROW = 0x27
CMD_MESSAGE = 0x28
CMD_LIGHT_RGB = 0x29
CMD_KEYS_CLEAR = 0x2A

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
HOST_MODES = range(16, 20)   # v2: host-defined modes

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
STRIP_TOUCH_BASE = 0x48   # note 0x48 / 0x49: pitch / mod strip touched
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
TEXT_POSITION = 0x04     # v2: song position as the host shows it
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
# v2 second TRANSPORT flags byte
TRANSPORT2_PUNCH = 1 << 0
TRANSPORT2_COUNT_IN = 1 << 1
TRANSPORT2_FOLLOW = 1 << 2

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
# v2
# ---------------------------------------------------------------------------

# HELLO features
FEATURE_PARAMETERS = 1 << 0   # PAGE / SLOT / VALUE instead of TEXT 0x01-0x03, 0x10-0x1F, PARAM
FEATURE_METERS = 1 << 1
FEATURE_SCALE = 1 << 2
FEATURE_KEY_LABELS = 1 << 3
FEATURE_LISTS = 1 << 4
FEATURE_MESSAGES = 1 << 5
FEATURE_COLOURS = 1 << 6
FEATURE_GRID = 1 << 7

# String budgets, in encoded bytes
MAX_HOST_NAME = 24
MAX_HOST_VERSION = 16
MAX_MODE_NAME = 16
MAX_PAGE_TEXT = 24       # PAGE title and subtitle
MAX_SLOT_NAME = 24
MAX_SLOT_GROUP = 16
MAX_VALUE_TEXT = 24
MAX_SCALE_NAME = 24
MAX_KEY_LABEL = 16
MAX_LIST_TITLE = 24
MAX_ROW_TEXT = 40
MAX_MESSAGE_TEXT = 64

POS_MAX = (1 << 14) - 1      # u14 normalised position
NO_COLOUR = (0, 0, 0)
BLACK = (1, 1, 1)            # hosts send black as 01 01 01

MODE_ENABLED = 1 << 0

# SLOT kind and flags
KIND_EMPTY = 0
KIND_CONTINUOUS = 1
KIND_TOGGLE = 2
KIND_ENUM = 3
SLOT_BIPOLAR = 1 << 0
SLOT_HAS_DEFAULT = 1 << 1

# METER
METER_SLOTS = 0          # target: the page's slots
METER_CHANNEL = 1        # target: the selected channel, L / R
METER_MASTER = 2
METER_SIGNAL = 0         # kind
METER_GAIN_REDUCTION = 1

# SCALE state
SCALE_NONE = 0
SCALE_SONG = 1
SCALE_CLIP = 2

# KEY roles
ROLE_NONE = 0
ROLE_KICK = 1
ROLE_SNARE = 2
ROLE_RIM = 3
ROLE_CLAP = 4
ROLE_CLOSED_HAT = 5
ROLE_PEDAL_HAT = 6
ROLE_OPEN_HAT = 7
ROLE_TOM = 8
ROLE_CYMBAL = 9
ROLE_PERCUSSION = 10
ROLE_OTHER = 11

# ROW flags
ROW_FOLDER = 1 << 0
ROW_PLAYING = 1 << 1
ROW_CHECKED = 1 << 2
ROW_DISABLED = 1 << 3

# MESSAGE flags
MESSAGE_LAST = 1 << 0
MESSAGE_BUSY = 1 << 1

# ---------------------------------------------------------------------------
# Events. parse() returns these for device -> host traffic, parse_host() for
# host -> device traffic. Unknown or malformed input returns None.
# ---------------------------------------------------------------------------

Hi = namedtuple("Hi", "proto")
# view: the device's current view (9th IDENT byte); None if older firmware
# left it out. The fields after it come with a v2 IDENT only (else None).
Ident = namedtuple("Ident", "proto board fw keys knobs caps view max_rx list_rows list_cache "
                            "msg_chars key_labels label_chars screen lights light_guide",
                   defaults=(None,) * 10)
View = namedtuple("View", "view")
Refresh = namedtuple("Refresh", "")
Description = namedtuple("Description", "part parts data")
Button = namedtuple("Button", "control pressed")
KnobTurn = namedtuple("KnobTurn", "knob delta")
KnobTouch = namedtuple("KnobTouch", "knob touched")
StripTouch = namedtuple("StripTouch", "strip touched")     # 0 pitch, 1 mod
EncoderTurn = namedtuple("EncoderTurn", "delta")

Hello = namedtuple("Hello", "proto features name version", defaults=(0, "", ""))
Ping = namedtuple("Ping", "")
Bye = namedtuple("Bye", "")
Text = namedtuple("Text", "field text")
Param = namedtuple("Param", "knob value flags")
Light = namedtuple("Light", "control state")
Transport = namedtuple("Transport", "flags tempo beat beats_per_bar flags2", defaults=(None,))
Grid = namedtuple("Grid", "cols rows cursor_col cursor_row states")
Notify = namedtuple("Notify", "title value")
# v2. Modes.entries: ((id, flags, name), ...); colours are (r, g, b) 7-bit.
Describe = namedtuple("Describe", "")
Modes = namedtuple("Modes", "entries")
SetMode = namedtuple("SetMode", "mode")
Page = namedtuple("Page", "gen mode bank banks title subtitle")
Slot = namedtuple("Slot", "gen knob kind flags steps default colour name group")
Value = namedtuple("Value", "gen knob pos text")
Meter = namedtuple("Meter", "target kind gen levels")
Scale = namedtuple("Scale", "state root mask name")
Key = namedtuple("Key", "note role colour label")
KeysClear = namedtuple("KeysClear", "")
ListHeader = namedtuple("ListHeader", "id total cursor first title")
Row = namedtuple("Row", "id index flags text")
Message = namedtuple("Message", "id part flags text")
LightRgb = namedtuple("LightRgb", "control state colour")

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


# -- v2 helpers --------------------------------------------------------------

def position(fraction):
    """0.0..1.0 -> u14 pos 0..16383 (rounded, clamped)."""
    return int(round(max(0.0, min(1.0, float(fraction))) * POS_MAX))


def u14(value):
    value = max(0, min(POS_MAX, int(value)))
    return [value >> 7, value & 0x7F]


def _u14_at(data, i):
    return (data[i] << 7) | data[i + 1]


def colour_bytes(colour):
    """(r, g, b) 7-bit -> three data bytes; None means no colour."""
    if colour is None:
        return list(NO_COLOUR)
    return [_data(c) for c in colour]


def _code_points(value):
    if value is None:
        return
    text = str(value).replace("\r\n", "\n").replace("\r", "\n").replace("\t", " ")
    for ch in text:
        code = ord(ch)
        yield 0x3F if 0xD800 <= code <= 0xDFFF else code   # lone surrogate -> '?'


def encode_string(value, budget):
    """A v2 string: length byte, then ASCII 0x20-0x7E as is, 0x0A for a line
    break, and any other code point as 7F hh mm ll. Cut at `budget` encoded
    bytes, never inside a character."""
    out = []
    for code in _code_points(value):
        if 0x20 <= code <= 0x7E or code == 0x0A:
            enc = [code]
        else:
            enc = [0x7F, (code >> 14) & 0x7F, (code >> 7) & 0x7F, code & 0x7F]
        if len(out) + len(enc) > budget:
            break
        out += enc
    return [len(out)] + out


def decode_string(data, i):
    """Decode the v2 string at data[i]. Returns (text, index after it)."""
    if i >= len(data):
        return "", i
    end = min(len(data), i + 1 + data[i])
    out = []
    j = i + 1
    while j < end:
        b = data[j]
        if b == 0x7F and j + 3 < end:
            code = (data[j + 1] << 14) | (data[j + 2] << 7) | data[j + 3]
            out.append(chr(code) if code <= 0x10FFFF else "?")
            j += 4
        elif b == 0x7F:
            break                       # truncated escape
        else:
            out.append(chr(b))
            j += 1
    return "".join(out), end


# ---------------------------------------------------------------------------
# Host -> device encoders (used by the script)
# ---------------------------------------------------------------------------

def hello(proto=PROTOCOL_VERSION, features=0, name="", version=""):
    """v1 HELLO by default; proto=2 adds features, host name and version."""
    if proto < PROTOCOL_V2:
        return _sysex(CMD_HELLO, (proto,))
    return _sysex(CMD_HELLO, [proto] + u14(features) + encode_string(name, MAX_HOST_NAME)
                  + encode_string(version, MAX_HOST_VERSION))


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


def transport(flags, bpm, beat, beats_per_bar, flags2=None):
    """flags2 (v2) is appended when given."""
    hi, lo = tempo_bytes(bpm)
    data = (flags & 0x3F, hi, lo, beat, beats_per_bar)
    if flags2 is not None:
        data += (flags2 & 0x07,)
    return _sysex(CMD_TRANSPORT, data)


def grid(cols, rows, cursor_col, cursor_row, states):
    states = list(states)
    if len(states) != cols * rows:
        raise ValueError("GRID needs cols x rows states")
    return _sysex(CMD_GRID, [cols, rows, cursor_col, cursor_row] + states)


def notify(title, value):
    data = _chars(title) + [0x00] + _chars(value)
    return _sysex(CMD_NOTIFY, data)


# -- v2 -----------------------------------------------------------------------

def describe():
    return _sysex(CMD_DESCRIBE)


def modes(entries):
    """entries: [(mode id, flags, name)]."""
    data = [len(entries)]
    for mode_id, flags, name in entries:
        data += [mode_id, flags & 0x7F] + encode_string(name, MAX_MODE_NAME)
    return _sysex(CMD_MODES, data)


def set_mode(mode_id):
    return _sysex(CMD_SET_MODE, (mode_id,))


def page(gen, mode, bank, banks, title, subtitle):
    return _sysex(CMD_PAGE, [gen, mode, bank, banks] + encode_string(title, MAX_PAGE_TEXT)
                  + encode_string(subtitle, MAX_PAGE_TEXT))


def slot(gen, knob, kind, flags, steps, default, colour, name, group):
    return _sysex(CMD_SLOT, [gen, knob, kind, flags & 0x03, steps] + u14(default)
                  + colour_bytes(colour) + encode_string(name, MAX_SLOT_NAME)
                  + encode_string(group, MAX_SLOT_GROUP))


def value(gen, knob, pos, value_text):
    """An empty value_text means "text unchanged"."""
    return _sysex(CMD_VALUE, [gen, knob] + u14(pos) + encode_string(value_text, MAX_VALUE_TEXT))


def meter(target, kind, gen, levels):
    levels = list(levels)
    return _sysex(CMD_METER, [target, kind, gen, len(levels)] + levels)


def scale(state, root, mask, name=""):
    return _sysex(CMD_SCALE, [state, root % 12] + u14(mask | 1) + encode_string(name, MAX_SCALE_NAME))


def key(note, role, colour, label):
    return _sysex(CMD_KEY, [note, role] + colour_bytes(colour) + encode_string(label, MAX_KEY_LABEL))


def keys_clear():
    return _sysex(CMD_KEYS_CLEAR)


def list_header(list_id, total, cursor, first, title):
    """LIST; list_id 0 closes the list."""
    return _sysex(CMD_LIST, [list_id] + u14(total) + u14(cursor) + u14(first)
                  + encode_string(title, MAX_LIST_TITLE))


def row(list_id, index, flags, row_text):
    return _sysex(CMD_ROW, [list_id] + u14(index) + [flags & 0x0F]
                  + encode_string(row_text, MAX_ROW_TEXT))


def message(message_id, part, flags, message_text):
    """MESSAGE; message_id 0 closes the message screen."""
    return _sysex(CMD_MESSAGE, [message_id, part, flags & 0x03]
                  + encode_string(message_text, MAX_MESSAGE_TEXT))


def light_rgb(control, state, colour):
    return _sysex(CMD_LIGHT_RGB, [control, state & 0x03] + colour_bytes(colour))


# ---------------------------------------------------------------------------
# Device -> host encoders (used by the simulator and the tests)
# ---------------------------------------------------------------------------

def hi():
    return _sysex(CMD_HI, (PROTOCOL_VERSION,))


def ident(board=1, fw=(0, 1, 0), keys=32, knobs=8, caps=0, view=None, proto=PROTOCOL_VERSION,
          max_rx=128, list_rows=4, list_cache=12, msg_chars=160, key_labels=16, label_chars=8,
          screen=1, lights=1, light_guide=0):
    """v1 IDENT by default; proto=2 appends the v2 fields (and needs view)."""
    data = (proto, board) + tuple(fw) + (keys, knobs, caps)
    if proto >= PROTOCOL_V2:
        data += (VIEW_INSTRUMENT if view is None else view,)
        data += tuple(u14(max_rx)) + (list_rows, list_cache) + tuple(u14(msg_chars))
        data += (key_labels, label_chars, screen, lights, light_guide)
    elif view is not None:
        data += (view,)
    return _sysex(CMD_IDENT, data)


def description(part, parts, json_bytes):
    return _sysex(CMD_DESCRIPTION, u14(part) + u14(parts) + list(json_bytes))


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
            view_id = None
            if len(data) >= 9 and (data[8] in VIEWS or data[8] in HOST_MODES):
                view_id = data[8]
            ext = ()
            if data[0] >= PROTOCOL_V2 and len(data) >= 20:
                ext = (_u14_at(data, 9), data[11], data[12], _u14_at(data, 13)) + tuple(data[15:20])
            return Ident(data[0], data[1], tuple(data[2:5]), data[5], data[6], data[7],
                         view_id, *ext)
        if cmd == CMD_VIEW and len(data) >= 1:
            return View(data[0])
        if cmd == CMD_REFRESH:
            return Refresh()
        if cmd == CMD_DESCRIPTION and len(data) >= 4:
            return Description(_u14_at(data, 0), _u14_at(data, 2), tuple(data[4:]))
        return None
    if len(b) != 3:
        return None
    status, num, value = b
    if status == NOTE_ON or status == NOTE_OFF:
        pressed = status == NOTE_ON and value > 0
        if num <= BUTTON_LAST:
            return Button(num, pressed)
        if STRIP_TOUCH_BASE <= num < STRIP_TOUCH_BASE + 2:
            return StripTouch(num - STRIP_TOUCH_BASE, pressed)
        if num < STRIP_TOUCH_BASE:
            return KnobTouch(num - KNOB_TOUCH_BASE, pressed)
        return None
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
        if data[0] >= PROTOCOL_V2 and len(data) >= 3:
            name, i = decode_string(data, 3)
            version, _ = decode_string(data, i)
            return Hello(data[0], _u14_at(data, 1), name, version)
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
        return Transport(data[0], ((data[1] << 7) | data[2]) / 10.0, data[3], data[4],
                         data[5] if len(data) >= 6 else None)
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
    return _parse_host_v2(cmd, data)


def _colour_at(data, i):
    return tuple(data[i:i + 3])


def _parse_host_v2(cmd, data):
    n = len(data)
    if cmd == CMD_DESCRIBE:
        return Describe()
    if cmd == CMD_MODES and n >= 1:
        entries, i = [], 1
        for _ in range(data[0]):
            if i + 2 > n:
                return None
            name, j = decode_string(data, i + 2)
            entries.append((data[i], data[i + 1], name))
            i = j
        return Modes(tuple(entries))
    if cmd == CMD_SET_MODE and n >= 1:
        return SetMode(data[0])
    if cmd == CMD_PAGE and n >= 4:
        title, i = decode_string(data, 4)
        subtitle, _ = decode_string(data, i)
        return Page(data[0], data[1], data[2], data[3], title, subtitle)
    if cmd == CMD_SLOT and n >= 10:
        name, i = decode_string(data, 10)
        group, _ = decode_string(data, i)
        return Slot(data[0], data[1], data[2], data[3], data[4], _u14_at(data, 5),
                    _colour_at(data, 7), name, group)
    if cmd == CMD_VALUE and n >= 4:
        return Value(data[0], data[1], _u14_at(data, 2), decode_string(data, 4)[0])
    if cmd == CMD_METER and n >= 4:
        return Meter(data[0], data[1], data[2], tuple(data[4:4 + data[3]]))
    if cmd == CMD_SCALE and n >= 4:
        return Scale(data[0], data[1], _u14_at(data, 2), decode_string(data, 4)[0])
    if cmd == CMD_KEY and n >= 5:
        return Key(data[0], data[1], _colour_at(data, 2), decode_string(data, 5)[0])
    if cmd == CMD_KEYS_CLEAR:
        return KeysClear()
    if cmd == CMD_LIST and n >= 7:
        return ListHeader(data[0], _u14_at(data, 1), _u14_at(data, 3), _u14_at(data, 5),
                          decode_string(data, 7)[0])
    if cmd == CMD_ROW and n >= 4:
        return Row(data[0], _u14_at(data, 1), data[3], decode_string(data, 4)[0])
    if cmd == CMD_MESSAGE and n >= 3:
        return Message(data[0], data[1], data[2], decode_string(data, 3)[0])
    if cmd == CMD_LIGHT_RGB and n >= 5:
        return LightRgb(data[0], data[1], _colour_at(data, 2))
    return None
