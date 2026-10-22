# Mixer view: the 8 knobs control the volume (Shift: pan) of 8 tracks.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   Knobs                 volume of 8 tracks (Shift held: pan)
#   4D turn               select track
#   4D left / right       bank tracks by 8
#   4D up / down          select previous / next track
#   4D push               arm selected track
#
# The bank covers visible tracks, then return tracks, then the master. When
# the selected track changes, the bank follows it so it stays on screen.

from . import protocol as p
from . import liveutil as lu
from .view_base import View

BANK = 8


class MixerView(View):

    def __init__(self, song, display):
        super(MixerView, self).__init__(song, display)
        self._offset = 0
        self._followed = None   # live_id of the selected track we last followed

    # -- model --------------------------------------------------------------

    def _tracks(self):
        tracks = lu.all_tracks(self._song)
        selected = lu.selected_track(self._song)
        sid = lu.live_id(selected) if selected is not None else None
        if sid != self._followed:
            # Selection moved (here or in Live): bring its bank on screen.
            self._followed = sid
            idx = lu.index_of(tracks, selected)
            if idx >= 0:
                self._offset = (idx // BANK) * BANK
        self._offset = max(0, min(self._offset, ((len(tracks) - 1) // BANK) * BANK))
        return tracks

    def _bank(self):
        tracks = self._tracks()
        bank = tracks[self._offset:self._offset + BANK]
        return tracks, bank + [None] * (BANK - len(bank))

    def knob_parameter(self, knob, shift):
        track = self._bank()[1][knob]
        if track is None:
            return None
        return lu.panning(track) if shift else lu.volume(track)

    # -- input --------------------------------------------------------------

    def enter(self):
        super(MixerView, self).enter()
        self._followed = None

    def encoder(self, delta, shift):
        lu.select_relative_track(self._song, delta)

    def press(self, control, shift):
        song = self._song
        if control in (p.CTL_NAV_LEFT, p.CTL_NAV_RIGHT):
            tracks = self._tracks()
            step = -BANK if control == p.CTL_NAV_LEFT else BANK
            new = self._offset + step
            if 0 <= new < len(tracks):
                self._offset = new
                self._nudger.reset()
        elif control == p.CTL_NAV_UP:
            lu.select_relative_track(song, -1)
        elif control == p.CTL_NAV_DOWN:
            lu.select_relative_track(song, 1)
        elif control == p.CTL_NAV_PUSH:
            track = lu.selected_track(song)
            armed = lu.toggle_arm(track)
            if armed is not None:
                self._display.notify(lu.safe_name(track), "Armed" if armed else "Disarmed")
        elif control in (p.CTL_PRESET_UP, p.CTL_PRESET_DOWN):
            pass   # unused in this view, but swallowed so nothing else acts
        else:
            return False
        return True

    # -- output -------------------------------------------------------------

    def render(self, shift):
        d = self._display
        self.render_header()
        tracks, bank = self._bank()
        d.text(p.TEXT_BANK, "Pan" if shift else "Volume")
        last = min(self._offset + BANK, len(tracks))
        d.text(p.TEXT_SUBTITLE, "Tracks %d-%d" % (self._offset + 1, last))
        for knob, track in enumerate(bank):
            if track is None:
                d.empty_knob(knob)
                continue
            param = lu.panning(track) if shift else lu.volume(track)
            lu.show_parameter(d, knob, param, label=lu.safe_name(track))

        song = self._song
        track = lu.selected_track(song)
        if lu.can_arm(track):
            push = p.LIGHT_ON if track.arm else p.LIGHT_DIM
        else:
            push = p.LIGHT_OFF
        self.render_nav_lights(up=lu.neighbour_exists(song, -1),
                               down=lu.neighbour_exists(song, 1),
                               left=self._offset > 0,
                               right=self._offset + BANK < len(tracks),
                               push=push)
        d.light(p.CTL_PRESET_UP, p.LIGHT_OFF)
        d.light(p.CTL_PRESET_DOWN, p.LIGHT_OFF)
