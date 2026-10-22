# Protocol-aware front end of the outbox: what views draw into.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Views call text() / param() / light() / grid() with plain values every
# render; the outbox turns that into "send only what changed". Priorities
# keep the time-critical messages (transport beat, button lights) ahead of a
# backlog of display text.

from . import protocol as p

PRIO_TRANSPORT = 0
PRIO_LIGHT = 1
PRIO_NOTIFY = 1
PRIO_GRID = 2
PRIO_PARAM = 2
PRIO_TEXT = 3


class Display(object):

    def __init__(self, outbox):
        self._outbox = outbox

    def text(self, field, value):
        self._outbox.put(("text", field), p.text(field, value), PRIO_TEXT)

    def param(self, knob, value, flags):
        self._outbox.put(("param", knob), p.param(knob, value, flags), PRIO_PARAM)

    def light(self, control, state):
        if control in p.DEVICE_OWNED_CONTROLS:
            return
        self._outbox.put(("light", control), p.light(control, state), PRIO_LIGHT)

    def transport(self, flags, bpm, beat, beats_per_bar):
        self._outbox.put(("transport",), p.transport(flags, bpm, beat, beats_per_bar),
                         PRIO_TRANSPORT)

    def grid(self, cols, rows, cursor_col, cursor_row, states):
        self._outbox.put(("grid",), p.grid(cols, rows, cursor_col, cursor_row, states),
                         PRIO_GRID)

    def notify(self, title, value):
        self._outbox.post(("notify",), p.notify(title, value), PRIO_NOTIFY)

    def knob(self, knob, label, value_text, value, flags):
        """Label, value text and bar for one knob in one call."""
        self.text(p.TEXT_KNOB_LABEL + knob, label)
        self.text(p.TEXT_KNOB_VALUE + knob, value_text)
        self.param(knob, value, flags)

    def empty_knob(self, knob):
        self.knob(knob, "", "", 0, 0)
