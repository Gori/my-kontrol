# Global buttons (same in every view), their lights, and TRANSPORT.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Button map (docs/protocol.md, "Suggested host behaviour"):
#
#   Play      play / stop             Shift: continue
#   Stop      stop                    Shift: stop all clips
#   Rec       arrangement record      Shift: session record
#   Loop      loop
#   Metro     metronome
#   Tempo     tap tempo
#   Undo      undo                    Shift: redo
#   Quantize  quantize selected clip
#   Ideas     capture MIDI
#   Mute      (device: previous knob mode)   Shift: mute selected track
#   Solo      (device: next knob mode)       Shift: solo selected track
#
# Without Shift the keyboard keeps Mute/Solo for itself and sends VIEW, so
# the script only sees them with Shift held. A press without Shift (older
# firmware) does the same thing.

from . import protocol as p
from . import liveutil as lu


class TransportComponent(object):

    def __init__(self, song, display):
        self._song = song
        self._display = display
        self._handlers = {
            p.CTL_PLAY: self._play,
            p.CTL_STOP: self._stop,
            p.CTL_REC: self._rec,
            p.CTL_LOOP: lambda shift: self._toggle("loop"),
            p.CTL_METRO: lambda shift: self._toggle("metronome"),
            p.CTL_TEMPO: lambda shift: lu.attempt(self._song.tap_tempo),
            p.CTL_UNDO: self._undo,
            p.CTL_QUANTIZE: self._quantize,
            p.CTL_IDEAS: self._capture,
            p.CTL_MUTE: self._mute,
            p.CTL_SOLO: self._solo,
        }

    def handles(self, control):
        return control in self._handlers

    def press(self, control, shift):
        handler = self._handlers.get(control)
        if handler is None:
            return False
        handler(shift)
        return True

    # -- actions ------------------------------------------------------------

    def _toggle(self, name):
        lu.set_attr(self._song, name, not getattr(self._song, name))

    def _play(self, shift):
        song = self._song
        if shift:
            lu.attempt(song.continue_playing)
        elif song.is_playing:
            lu.attempt(song.stop_playing)
        else:
            lu.attempt(song.start_playing)

    def _stop(self, shift):
        if shift:
            lu.attempt(self._song.stop_all_clips)
        else:
            lu.attempt(self._song.stop_playing)

    def _rec(self, shift):
        self._toggle("session_record" if shift else "record_mode")

    def _undo(self, shift):
        song = self._song
        if shift:
            if song.can_redo and lu.attempt(song.redo):
                self._display.notify("Redo", "")
        elif song.can_undo and lu.attempt(song.undo):
            self._display.notify("Undo", "")

    def _quantize(self, shift):
        clip = lu.selected_clip(self._song)
        if clip is None:
            self._display.notify("Quantize", "No clip")
            return
        label = lu.quantize_clip(self._song, clip)
        if label:
            self._display.notify("Quantize", label)

    def _capture(self, shift):
        song = self._song
        if getattr(song, "can_capture_midi", False) and lu.attempt(song.capture_midi):
            self._display.notify("Capture", "MIDI")

    def _mute(self, shift):
        track = lu.selected_track(self._song)
        if lu.has_mute(self._song, track):
            lu.set_attr(track, "mute", not track.mute)

    def _solo(self, shift):
        track = lu.selected_track(self._song)
        if lu.has_mute(self._song, track):
            lu.set_attr(track, "solo", not track.solo)

    # -- feedback -----------------------------------------------------------

    def beat(self):
        """0-based beat in the bar, or BEAT_STOPPED."""
        song = self._song
        if not song.is_playing:
            return p.BEAT_STOPPED
        try:
            return max(0, min(126, song.get_current_beats_song_time().beats - 1))
        except Exception:
            return 0

    def render_transport(self):
        song = self._song
        flags = 0
        for attr, bit in (("is_playing", p.TRANSPORT_PLAYING),
                          ("record_mode", p.TRANSPORT_ARRANGEMENT_REC),
                          ("session_record", p.TRANSPORT_SESSION_REC),
                          ("loop", p.TRANSPORT_LOOP),
                          ("metronome", p.TRANSPORT_METRONOME),
                          ("can_capture_midi", p.TRANSPORT_CAPTURE)):
            if getattr(song, attr, False):
                flags |= bit
        self._display.transport(flags, song.tempo, self.beat(), song.signature_numerator)

    def render_lights(self):
        song, d = self._song, self._display
        on_dim = lambda v: p.LIGHT_ON if v else p.LIGHT_DIM
        d.light(p.CTL_PLAY, on_dim(song.is_playing))
        d.light(p.CTL_STOP, p.LIGHT_DIM)
        if song.record_mode:
            d.light(p.CTL_REC, p.LIGHT_ON)
        else:
            d.light(p.CTL_REC, p.LIGHT_BLINK if song.session_record else p.LIGHT_DIM)
        d.light(p.CTL_LOOP, on_dim(song.loop))
        d.light(p.CTL_METRO, on_dim(song.metronome))
        d.light(p.CTL_TEMPO, p.LIGHT_DIM)   # the device flashes it on each beat
        d.light(p.CTL_UNDO, p.LIGHT_DIM if (song.can_undo or song.can_redo) else p.LIGHT_OFF)
        d.light(p.CTL_QUANTIZE,
                p.LIGHT_DIM if lu.selected_clip(song) is not None else p.LIGHT_OFF)
        d.light(p.CTL_IDEAS,
                p.LIGHT_ON if getattr(song, "can_capture_midi", False) else p.LIGHT_OFF)
        track = lu.selected_track(song)
        if lu.has_mute(song, track):
            d.light(p.CTL_MUTE, on_dim(track.mute))
            d.light(p.CTL_SOLO, on_dim(track.solo))
        else:
            d.light(p.CTL_MUTE, p.LIGHT_OFF)
            d.light(p.CTL_SOLO, p.LIGHT_OFF)

    def render(self):
        self.render_transport()
        self.render_lights()
