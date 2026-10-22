# Live glue: a _Framework ControlSurface that hands everything to Engine.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Why _Framework: it has been frozen since Live 9, is still shipped and used
# by many bundled scripts in Live 12, and is the best documented of Live's
# script frameworks. We only use its ControlSurface shell (MIDI in/out,
# 100 ms timer, component guard); all behaviour lives in engine.py and below,
# which do not import Live.

import time
import traceback

import Live
from _Framework.ControlSurface import ControlSurface

from . import protocol as p
from .engine import Engine

PORT_NAME = "My Kontrol DAW"

# Notes and CCs on channel 16 that Live must forward to the script instead
# of treating them as MIDI-mappable input.
_FORWARD_NOTES = range(0, 0x48)                       # buttons 0..63, knob touch 0x40..0x47
_FORWARD_CCS = list(range(p.KNOB_CC_BASE, p.KNOB_CC_BASE + 8)) + [p.ENCODER_CC]


def _live_banks():
    """Banks backed by Live's own bank definitions (the tables Push and the
    other bundled scripts use), or plain 8-at-a-time banks without them."""
    from .banks import Banks
    for module in ("ableton.v3.control_surface", "ableton.v2.control_surface.default_bank_definitions"):
        try:
            definitions = __import__(module, fromlist=["BANK_DEFINITIONS"]).BANK_DEFINITIONS
            return Banks(definitions)
        except Exception:
            continue
    return Banks()


class MyKontrol(ControlSurface):

    def __init__(self, c_instance):
        super(MyKontrol, self).__init__(c_instance)
        self._suggested_input_port = PORT_NAME
        self._suggested_output_port = PORT_NAME
        with self.component_guard():
            self._engine = Engine(self.song(),
                                  send=self._send_bytes,
                                  clock=time.monotonic,
                                  banks=_live_banks(),
                                  highlight=self._highlight,
                                  log=self.log_message)
            self.song().add_current_song_time_listener(self._on_song_time)
        self._guarded(self._engine.start)
        self.log_message("My Kontrol script loaded")

    # -- helpers ------------------------------------------------------------

    def _guarded(self, fn, *args):
        """Run engine code; log instead of killing the script on a bug."""
        try:
            fn(*args)
        except Exception:
            self.log_message("error:\n" + traceback.format_exc())

    def _send_bytes(self, message):
        # Bypass _Framework's accumulate-and-deduplicate path: the outbox
        # already deduplicates and orders our messages.
        self._do_send_midi(tuple(message))

    def _highlight(self, track_offset, scene_offset, width, height, include_returns):
        try:
            self._c_instance.set_session_highlight(track_offset, scene_offset, width,
                                                   height, include_returns)
        except Exception:
            pass

    # -- Live callbacks -----------------------------------------------------

    def build_midi_map(self, midi_map_handle):
        super(MyKontrol, self).build_midi_map(midi_map_handle)
        handle = self._c_instance.handle()
        for note in _FORWARD_NOTES:
            Live.MidiMap.forward_midi_note(handle, midi_map_handle, p.CONTROL_CHANNEL, note)
        for cc in _FORWARD_CCS:
            Live.MidiMap.forward_midi_cc(handle, midi_map_handle, p.CONTROL_CHANNEL, cc)

    def handle_sysex(self, midi_bytes):
        self._guarded(self._engine.receive, midi_bytes)

    def handle_nonsysex(self, midi_bytes):
        self._guarded(self._engine.receive, midi_bytes)

    def update_display(self):
        super(MyKontrol, self).update_display()
        with self.component_guard():
            self._guarded(self._engine.tick)

    def _on_song_time(self):
        self._guarded(self._engine.song_time_changed)

    # _Framework already listens to Song.View.selected_track / selected_scene
    # and calls these; hook them for prompt redraws.
    def _on_selected_track_changed(self):
        super(MyKontrol, self)._on_selected_track_changed()
        if getattr(self, "_engine", None) is not None:
            self._guarded(self._engine.selection_changed)

    def _on_selected_scene_changed(self):
        super(MyKontrol, self)._on_selected_scene_changed()
        if getattr(self, "_engine", None) is not None:
            self._guarded(self._engine.selection_changed)

    def refresh_state(self):
        super(MyKontrol, self).refresh_state()
        self._guarded(self._engine.full_refresh)

    def port_settings_changed(self):
        super(MyKontrol, self).port_settings_changed()
        # New ports: start over with an unsolicited HELLO.
        self._guarded(self._engine.start)

    def disconnect(self):
        song = self.song()
        if song.current_song_time_has_listener(self._on_song_time):
            song.remove_current_song_time_listener(self._on_song_time)
        self._guarded(self._engine.stop)
        super(MyKontrol, self).disconnect()
