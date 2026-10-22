# Sends view: the 8 knobs control sends A-H of the selected track.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   Knobs                 sends A-H of the selected track (as many as exist)
#   4D turn / arrows      select track
#   4D push               arm selected track
#
# Labels are the return track names, values Live's dB text.

from . import protocol as p
from . import liveutil as lu
from .mixer_view import TrackSelectView


class SendsView(TrackSelectView):

    def _sends(self):
        sends = lu.sends(lu.selected_track(self._song))[:8]
        return sends + [None] * (8 - len(sends))

    def knob_parameter(self, knob, shift):
        return self._sends()[knob]

    def press(self, control, shift):
        if control in (p.CTL_PRESET_UP, p.CTL_PRESET_DOWN):
            return True   # unused here, but swallowed so nothing else acts
        return super(SendsView, self).press(control, shift)

    def render(self, shift):
        d = self._display
        self.render_header()
        sends = self._sends()
        count = sum(1 for s in sends if s is not None)
        returns = list(self._song.return_tracks)
        d.text(p.TEXT_BANK, "")
        if count == 0:
            subtitle = "No sends"
        elif count == 1:
            subtitle = "Send A"
        else:
            subtitle = "Sends A-%s" % chr(ord("A") + count - 1)
        d.text(p.TEXT_SUBTITLE, subtitle)
        for knob, send in enumerate(sends):
            if send is None:
                d.empty_knob(knob)
            else:
                name = lu.safe_name(returns[knob]) if knob < len(returns) else lu.safe_name(send)
                lu.show_parameter(d, knob, send, label=name)
        self.render_track_nav_lights()
        d.light(p.CTL_PRESET_UP, p.LIGHT_OFF)
        d.light(p.CTL_PRESET_DOWN, p.LIGHT_OFF)
