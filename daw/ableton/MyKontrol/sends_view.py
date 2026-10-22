# Sends view: the 8 knobs control sends A-H of the selected track.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   Knobs                 sends A-H of the selected track (as many as exist)
#   4D turn / arrows      select track
#   4D push               arm selected track
#
# Labels are the return track names, values Live's dB text. v2: PAGE with
# no title (the keyboard shows its own), knobs in the return tracks'
# colours, slot meters show the return tracks' output.

from . import protocol as p
from . import liveutil as lu
from .mixer_view import TrackSelectView


class SendsView(TrackSelectView):

    def _sends(self):
        sends = lu.sends(lu.selected_track(self._song))[:8]
        return sends + [None] * (8 - len(sends))

    def knob_parameter(self, knob, shift):
        return self._sends()[knob]

    def meter_tracks(self):
        returns = list(self._song.return_tracks)[:8]
        return [r if s is not None else None
                for r, s in zip(returns + [None] * (8 - len(returns)), self._sends())]

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
        if count == 0:
            subtitle = "No sends"
        elif count == 1:
            subtitle = "Send A"
        else:
            subtitle = "Sends A-%s" % chr(ord("A") + count - 1)
        track = lu.selected_track(self._song)
        d.page(self.mode, "", subtitle,
               identity=(lu.live_id(track) if track is not None else None, count),
               v1=(self.device_text(), "", subtitle))
        for knob, send in enumerate(sends):
            if send is None:
                d.empty_knob(knob)
            elif knob < len(returns):
                lu.show_parameter(d, knob, send, label=lu.safe_name(returns[knob]),
                                  colour=lu.colour(returns[knob]))
            else:
                lu.show_parameter(d, knob, send, label=lu.safe_name(send))
        self.render_track_nav_lights()
        d.light(p.CTL_PRESET_UP, p.LIGHT_OFF)
        d.light(p.CTL_PRESET_DOWN, p.LIGHT_OFF)
