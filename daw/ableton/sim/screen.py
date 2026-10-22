# What a My Kontrol keyboard knows after receiving host messages.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Applies decoded host -> device messages to a plain state model. Used by the
# simulator (mk_sim.py) to draw a status screen and by the unit tests to
# check what the device would end up showing. No MIDI library needed.

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from MyKontrol import protocol as p  # noqa: E402

TEXT_FIELD_NAMES = {p.TEXT_TRACK: "track", p.TEXT_DEVICE: "device",
                    p.TEXT_BANK: "bank", p.TEXT_SUBTITLE: "subtitle"}
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

    def __init__(self):
        self.reset()

    def reset(self):
        self.texts = {}
        self.params = {}
        self.lights = {}
        self.transport = None
        self.grid = None
        self.notifications = []
        self.hello = 0
        self.pings = 0
        self.byes = 0

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
        elif isinstance(event, p.Ping):
            self.pings += 1
        elif isinstance(event, p.Bye):
            self.byes += 1

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
        return "HELLO     proto %d" % event.proto
    if isinstance(event, p.Ping):
        return "PING"
    if isinstance(event, p.Bye):
        return "BYE"
    return repr(event)
