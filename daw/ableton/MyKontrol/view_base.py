# Base class for the views (device, volume, pan, sends, clips).
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A view maps the view-dependent controls (knobs, 4D encoder, Preset up/down)
# and draws its part of the display. Rendering is "immediate mode": render()
# writes every field the view owns, every time, and the outbox drops what the
# device already shows. That keeps views free of listener bookkeeping.
#
# `mode` is the view's mode ID (VIEW / MODES), set by the controller.

from . import protocol as p
from . import liveutil as lu


class View(object):

    mode = None
    owns_list = False     # True while the view shows a v2 LIST

    def __init__(self, song, display):
        self._song = song
        self._display = display
        self._nudger = lu.Nudger()
        self._touched = {}   # knob -> parameter we began a gesture on

    # -- lifecycle ----------------------------------------------------------

    def enter(self):
        self._nudger.reset()

    def exit(self):
        for knob in list(self._touched):
            self.touch(knob, False, False)

    # -- input --------------------------------------------------------------

    def knob_parameter(self, knob, shift):
        """The parameter knob controls right now, or None."""
        return None

    def turn(self, knob, delta, shift):
        return self._nudger.nudge(knob, self.knob_parameter(knob, shift), delta)

    def touch(self, knob, touched, shift):
        # Touch brackets an automation gesture, so recording automation with
        # the knobs writes one clean ramp instead of a stream of jumps.
        if touched:
            param = self.knob_parameter(knob, shift)
            if param is not None:
                lu.begin_gesture(param)
                self._touched[knob] = param
                if shift:
                    lu.reset_to_default(param)
        else:
            lu.end_gesture(self._touched.pop(knob, None))

    def encoder(self, delta, shift):
        """4D encoder turned."""

    def press(self, control, shift):
        """A view-dependent button. Returns True if handled."""
        return False

    # -- output -------------------------------------------------------------

    def render(self, shift):
        raise NotImplementedError

    def meter_tracks(self):
        """The tracks whose output the slot meters show (v2), or None."""
        return None

    def render_header(self):
        track = lu.selected_track(self._song)
        self._display.text(p.TEXT_TRACK, lu.safe_name(track))

    def device_text(self):
        """v1 TEXT_DEVICE in the mixer and clip views."""
        return lu.safe_name(lu.selected_device(self._song))

    def render_nav_lights(self, up, down, left, right, push):
        d = self._display
        dim = lambda ok: p.LIGHT_DIM if ok else p.LIGHT_OFF
        d.light(p.CTL_NAV_UP, dim(up))
        d.light(p.CTL_NAV_DOWN, dim(down))
        d.light(p.CTL_NAV_LEFT, dim(left))
        d.light(p.CTL_NAV_RIGHT, dim(right))
        d.light(p.CTL_NAV_PUSH, push)
