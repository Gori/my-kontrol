# Volume and pan views: the 8 knobs control the volume or pan of 8 tracks.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   Knobs                 volume (volume view) / pan (pan view) of 8 tracks
#   4D turn               select track
#   4D left/right/up/down select previous / next track
#   4D push               arm selected track
#   Preset up / down      tracks 1-8 / 9-16 / ...
#
# The pages cover visible tracks, then return tracks, then the master. When
# the selected track changes, the page follows it so it stays on screen.
# The keyboard draws the mode name itself; we only send the page subtitle.

from . import protocol as p
from . import liveutil as lu
from .view_base import View

PAGE = 8


class TrackSelectView(View):
    """Shared by the volume, pan and sends views: the 4D encoder selects
    tracks and its push arms the selected track."""

    def encoder(self, delta, shift):
        lu.select_relative_track(self._song, delta)

    def press(self, control, shift):
        song = self._song
        if control in (p.CTL_NAV_LEFT, p.CTL_NAV_UP):
            lu.select_relative_track(song, -1)
        elif control in (p.CTL_NAV_RIGHT, p.CTL_NAV_DOWN):
            lu.select_relative_track(song, 1)
        elif control == p.CTL_NAV_PUSH:
            track = lu.selected_track(song)
            armed = lu.toggle_arm(track)
            if armed is not None:
                self._display.notify(lu.safe_name(track), "Armed" if armed else "Disarmed")
        else:
            return False
        return True

    def render_track_nav_lights(self):
        song = self._song
        track = lu.selected_track(song)
        if lu.can_arm(track):
            push = p.LIGHT_ON if track.arm else p.LIGHT_DIM
        else:
            push = p.LIGHT_OFF
        back, forward = lu.neighbour_exists(song, -1), lu.neighbour_exists(song, 1)
        self.render_nav_lights(up=back, down=forward, left=back, right=forward, push=push)


class MixerView(TrackSelectView):

    def __init__(self, song, display, pan=False):
        super(MixerView, self).__init__(song, display)
        self._pan = pan
        self._offset = 0
        self._followed = None   # live_id of the selected track we last followed

    # -- model --------------------------------------------------------------

    def _tracks(self):
        tracks = lu.all_tracks(self._song)
        selected = lu.selected_track(self._song)
        sid = lu.live_id(selected) if selected is not None else None
        if sid != self._followed:
            # Selection moved (here or in Live): bring its page on screen.
            self._followed = sid
            idx = lu.index_of(tracks, selected)
            if idx >= 0:
                self._offset = (idx // PAGE) * PAGE
        self._offset = max(0, min(self._offset, ((len(tracks) - 1) // PAGE) * PAGE))
        return tracks

    def _page(self):
        tracks = self._tracks()
        page = tracks[self._offset:self._offset + PAGE]
        return tracks, page + [None] * (PAGE - len(page))

    def _param(self, track):
        if track is None:
            return None
        return lu.panning(track) if self._pan else lu.volume(track)

    def knob_parameter(self, knob, shift):
        return self._param(self._page()[1][knob])

    # -- input --------------------------------------------------------------

    def enter(self):
        super(MixerView, self).enter()
        self._followed = None

    def press(self, control, shift):
        if control in (p.CTL_PRESET_UP, p.CTL_PRESET_DOWN):
            tracks = self._tracks()
            new = self._offset + (-PAGE if control == p.CTL_PRESET_UP else PAGE)
            if 0 <= new < len(tracks):
                self._offset = new
                self._nudger.reset()
            return True
        return super(MixerView, self).press(control, shift)

    # -- output -------------------------------------------------------------

    def render(self, shift):
        d = self._display
        self.render_header()
        tracks, page = self._page()
        d.text(p.TEXT_BANK, "")
        last = min(self._offset + PAGE, len(tracks))
        d.text(p.TEXT_SUBTITLE, "Tracks %d-%d" % (self._offset + 1, last))
        for knob, track in enumerate(page):
            if track is None:
                d.empty_knob(knob)
            else:
                lu.show_parameter(d, knob, self._param(track), label=lu.safe_name(track))
        self.render_track_nav_lights()
        dim = lambda ok: p.LIGHT_DIM if ok else p.LIGHT_OFF
        d.light(p.CTL_PRESET_UP, dim(self._offset > 0))
        d.light(p.CTL_PRESET_DOWN, dim(self._offset + PAGE < len(tracks)))
