# What a My Kontrol keyboard knows after receiving host messages.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Applies decoded host -> device messages to a plain state model. Used by the
# simulator (mk_sim.py) to draw a status screen and by the unit tests to
# check what the device would end up showing. No MIDI library needed.
#
# v2 follows docs/protocol.md as the keyboard would: PAGE clears the
# slots, SLOT / VALUE / slot METER with another gen are dropped (counted in
# `dropped`), an empty VALUE text keeps the text, the keyboard keeps the
# first `key_labels` KEYs, and the list cache holds `list_cache` rows and
# evicts the farthest from the cursor; of two equally far rows the one
# with the higher index, which may be the new row itself. Ties are counted
# in `list_ties`, so tests can check the host never relies on them.

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from MyKontrol import protocol as p  # noqa: E402

TEXT_FIELD_NAMES = {p.TEXT_TRACK: "track", p.TEXT_DEVICE: "device",
                    p.TEXT_BANK: "bank", p.TEXT_SUBTITLE: "subtitle",
                    p.TEXT_POSITION: "position"}
for _k in range(8):
    TEXT_FIELD_NAMES[p.TEXT_KNOB_LABEL + _k] = "knob%d.label" % (_k + 1)
    TEXT_FIELD_NAMES[p.TEXT_KNOB_VALUE + _k] = "knob%d.value" % (_k + 1)
    TEXT_FIELD_NAMES[p.TEXT_COLUMN + _k] = "col%d" % (_k + 1)
for _k in range(4):
    TEXT_FIELD_NAMES[p.TEXT_ROW + _k] = "row%d" % (_k + 1)

LIGHT_NAMES = {p.LIGHT_OFF: "off", p.LIGHT_DIM: "dim", p.LIGHT_ON: "on", p.LIGHT_BLINK: "blink"}
SLOT_CHARS = {p.SLOT_NONE: " ", p.SLOT_EMPTY: ".", p.SLOT_STOPPED: "o",
              p.SLOT_PLAYING: ">", p.SLOT_RECORDING: "R", p.SLOT_QUEUED: "~",
              p.SLOT_QUEUED_RECORD: "r", p.SLOT_ARMED: "_"}


class Screen(object):

    def __init__(self, list_rows=4, list_cache=12, key_labels=16):
        self.list_rows = list_rows
        self.list_cache = list_cache
        self.key_labels = key_labels
        self.mode = None          # the keyboard's mode; None accepts any PAGE
        self.reset()

    def reset(self):
        self.texts = {}
        self.params = {}
        self.lights = {}
        self.transport = None
        self.grid = None
        self.notifications = []
        self.hello = 0
        self.last_hello = None
        self.pings = 0
        self.byes = 0
        self.modes = None
        self.set_modes = []
        self.clear_v2()

    def clear_v2(self):
        """What BYE clears (v2)."""
        self.page = None
        self.slots = {}
        self.values = {}
        self.meters = {}
        self.scale = None
        self.keys = []            # [Key], in arrival order
        self.keys_cleared = 0
        self.list = None
        self.rows = {}
        self.message = None
        self.dropped = 0
        self.list_ties = 0

    # -- v2 helpers ---------------------------------------------------------

    def _gen_ok(self, gen):
        if self.page is None or gen != self.page.gen:
            self.dropped += 1
            return False
        return True

    def _add_row(self, row):
        cursor = self.list.cursor
        self.rows[row.index] = row
        while len(self.rows) > self.list_cache:
            far = max(abs(i - cursor) for i in self.rows)
            candidates = sorted(i for i in self.rows if abs(i - cursor) == far)
            if len(candidates) > 1:
                self.list_ties += 1
            del self.rows[candidates[-1]]

    def key_for(self, note):
        for k in self.keys:
            if k.note == note:
                return k
        return None

    def value_text(self, knob):
        v = self.values.get(knob)
        return v[1] if v else None

    def visible_rows(self):
        if self.list is None:
            return []
        first = self.list.first
        last = min(self.list.total, first + self.list_rows)
        return [self.rows.get(i) for i in range(first, last)]

    def apply(self, event):
        """Apply one protocol.parse_host() event."""
        if isinstance(event, p.Text):
            self.texts[event.field] = event.text
        elif isinstance(event, p.Param):
            self.params[event.knob] = (event.value, event.flags)
        elif isinstance(event, p.Light):
            self.lights[event.control] = event.state
        elif isinstance(event, p.Transport):
            self.transport = event
        elif isinstance(event, p.Grid):
            self.grid = event
        elif isinstance(event, p.Notify):
            self.notifications.append((event.title, event.value))
        elif isinstance(event, p.Hello):
            self.hello += 1
            self.last_hello = event
        elif isinstance(event, p.Ping):
            self.pings += 1
        elif isinstance(event, p.Bye):
            self.byes += 1
            self.clear_v2()
        else:
            self._apply_v2(event)

    def _apply_v2(self, event):
        if isinstance(event, p.Modes):
            self.modes = event.entries
        elif isinstance(event, p.SetMode):
            self.set_modes.append(event.mode)
        elif isinstance(event, p.Page):
            if self.mode is not None and event.mode != self.mode:
                self.dropped += 1
                return
            self.page = event
            self.slots = {}
            self.values = {}
            self.meters.pop(p.METER_SLOTS, None)
        elif isinstance(event, p.Slot):
            if self._gen_ok(event.gen):
                self.slots[event.knob] = event
        elif isinstance(event, p.Value):
            if self._gen_ok(event.gen):
                old = self.values.get(event.knob)
                text = event.text if event.text or old is None else old[1]
                self.values[event.knob] = (event.pos, text)
        elif isinstance(event, p.Meter):
            if event.target != p.METER_SLOTS or self._gen_ok(event.gen):
                self.meters[event.target] = event
        elif isinstance(event, p.Scale):
            self.scale = event
        elif isinstance(event, p.KeysClear):
            self.keys = []
            self.keys_cleared += 1
        elif isinstance(event, p.Key):
            self.keys = [k for k in self.keys if k.note != event.note]
            if event.role or event.label:
                if len(self.keys) < self.key_labels:
                    self.keys.append(event)
        elif isinstance(event, p.ListHeader):
            if event.id == 0:
                self.list, self.rows = None, {}
                return
            if self.list is None or self.list.id != event.id:
                self.rows = {}
            self.list = event
        elif isinstance(event, p.Row):
            if self.list is not None and event.id == self.list.id:
                self._add_row(event)
        elif isinstance(event, p.Message):
            self.message = None if event.id == 0 else event

    def feed(self, midi_bytes):
        event = p.parse_host(midi_bytes)
        if event is not None:
            self.apply(event)
        return event

    def text(self, field):
        return self.texts.get(field)

    def grid_rows(self):
        if self.grid is None:
            return []
        g = self.grid
        rows = []
        for r in range(g.rows):
            cells = []
            for c in range(g.cols):
                ch = SLOT_CHARS.get(g.states[r * g.cols + c], "?")
                if (c, r) == (g.cursor_col, g.cursor_row):
                    ch = "[" + ch + "]"
                else:
                    ch = " " + ch + " "
                cells.append(ch)
            rows.append("".join(cells))
        return rows


def describe(event):
    """One human-readable line for a host -> device event."""
    if isinstance(event, p.Text):
        name = TEXT_FIELD_NAMES.get(event.field, "0x%02X" % event.field)
        return "TEXT      %-12s %r" % (name, event.text)
    if isinstance(event, p.Param):
        flags = "".join(f for bit, f in ((p.PARAM_MAPPED, "M"), (p.PARAM_BIPOLAR, "B"),
                                         (p.PARAM_STEPPED, "S")) if event.flags & bit)
        bar = "#" * (event.value * 16 // 127)
        return "PARAM     knob%d %3d %-3s |%-16s|" % (event.knob + 1, event.value, flags, bar)
    if isinstance(event, p.Light):
        name = p.CONTROL_NAMES.get(event.control, str(event.control))
        return "LIGHT     %-12s %s" % (name, LIGHT_NAMES.get(event.state, event.state))
    if isinstance(event, p.Transport):
        flags = [n for bit, n in ((p.TRANSPORT_PLAYING, "play"),
                                  (p.TRANSPORT_ARRANGEMENT_REC, "rec"),
                                  (p.TRANSPORT_SESSION_REC, "session-rec"),
                                  (p.TRANSPORT_LOOP, "loop"),
                                  (p.TRANSPORT_METRONOME, "metro"),
                                  (p.TRANSPORT_CAPTURE, "capture")) if event.flags & bit]
        flags += [n for bit, n in ((p.TRANSPORT2_PUNCH, "punch"),
                                   (p.TRANSPORT2_COUNT_IN, "count-in"),
                                   (p.TRANSPORT2_FOLLOW, "follow")) if (event.flags2 or 0) & bit]
        beat = "-" if event.beat == p.BEAT_STOPPED else str(event.beat + 1)
        return "TRANSPORT %.1f BPM beat %s/%d %s" % (event.tempo, beat, event.beats_per_bar,
                                                   " ".join(flags))
    if isinstance(event, p.Grid):
        s = Screen()
        s.grid = event
        return "GRID      %dx%d cursor %d,%d\n          %s" % (
            event.cols, event.rows, event.cursor_col, event.cursor_row,
            "\n          ".join(s.grid_rows()))
    if isinstance(event, p.Notify):
        return "NOTIFY    %r / %r" % (event.title, event.value)
    if isinstance(event, p.Hello):
        if event.proto >= p.PROTOCOL_V2:
            return "HELLO     proto %d features 0x%02X %r %r" % (event.proto, event.features,
                                                              event.name, event.version)
        return "HELLO     proto %d" % event.proto
    if isinstance(event, p.Modes):
        return "MODES     " + " ".join("%d%s%s" % (m, "" if f & p.MODE_ENABLED else "(off)",
                                                   ":" + n if n else "")
                                       for m, f, n in event.entries)
    if isinstance(event, p.SetMode):
        return "SET_MODE  %s" % p.VIEW_NAMES.get(event.mode, event.mode)
    if isinstance(event, p.Page):
        return "PAGE      gen %d mode %d bank %d/%d %r / %r" % (
            event.gen, event.mode, event.bank + 1, event.banks, event.title, event.subtitle)
    if isinstance(event, p.Slot):
        kind = {p.KIND_EMPTY: "empty", p.KIND_CONTINUOUS: "cont", p.KIND_TOGGLE: "toggle",
                p.KIND_ENUM: "enum"}.get(event.kind, event.kind)
        return "SLOT      gen %d knob%d %-6s flags %d steps %d default %d rgb %s %r %r" % (
            event.gen, event.knob + 1, kind, event.flags, event.steps, event.default,
            event.colour, event.name, event.group)
    if isinstance(event, p.Value):
        bar = "#" * (event.pos * 16 // p.POS_MAX)
        return "VALUE     gen %d knob%d %5d |%-16s| %r" % (event.gen, event.knob + 1, event.pos,
                                                        bar, event.text)
    if isinstance(event, p.Meter):
        return "METER     target %d kind %d gen %d %s" % (event.target, event.kind, event.gen,
                                                        list(event.levels))
    if isinstance(event, p.Scale):
        return "SCALE     state %d root %d mask %s %r" % (event.state, event.root,
                                                       format(event.mask, "012b")[::-1],
                                                       event.name)
    if isinstance(event, p.Key):
        return "KEY       note %d role %d rgb %s %r" % (event.note, event.role, event.colour,
                                                      event.label)
    if isinstance(event, p.KeysClear):
        return "KEYS_CLEAR"
    if isinstance(event, p.ListHeader):
        if event.id == 0:
            return "LIST      closed"
        return "LIST      id %d %r cursor %d/%d first %d" % (event.id, event.title, event.cursor,
                                                           event.total, event.first)
    if isinstance(event, p.Row):
        return "ROW       id %d %4d %s %r" % (event.id, event.index,
                                             "/" if event.flags & p.ROW_FOLDER else " ",
                                             event.text)
    if isinstance(event, p.Message):
        if event.id == 0:
            return "MESSAGE   closed"
        return "MESSAGE   id %d part %d flags %d %r" % (event.id, event.part, event.flags,
                                                     event.text)
    if isinstance(event, p.Ping):
        return "PING"
    if isinstance(event, p.Bye):
        return "BYE"
    return repr(event)
