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

PRIO_TRANSPORT = 0    # beat flashes
PRIO_HOT = 0          # the knob being turned / touched: name and value
PRIO_LIGHT = 1        # 1 and up is "bulk" traffic; see Outbox.reserve
PRIO_NOTIFY = 1
PRIO_GRID = 2
PRIO_PARAM = 2
PRIO_TEXT = 3


class Display(object):

    def __init__(self, outbox):
        self._outbox = outbox
        self.hot_knob = None   # set by the controller on knob turn / touch

    def text(self, field, value, priority=PRIO_TEXT):
        self._outbox.put(("text", field), p.text(field, value), priority)

    def param(self, knob, value, flags, priority=PRIO_PARAM):
        self._outbox.put(("param", knob), p.param(knob, value, flags), priority)

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
        """Label, value text and bar for one knob in one call. The knob in
        use jumps the queue so its value text keeps up with turning."""
        hot = knob == self.hot_knob
        self.text(p.TEXT_KNOB_LABEL + knob, label, PRIO_HOT if hot else PRIO_TEXT)
        self.text(p.TEXT_KNOB_VALUE + knob, value_text, PRIO_HOT if hot else PRIO_PARAM)
        self.param(knob, value, flags, PRIO_HOT if hot else PRIO_PARAM)

    def empty_knob(self, knob):
        self.knob(knob, "", "", 0, 0)
