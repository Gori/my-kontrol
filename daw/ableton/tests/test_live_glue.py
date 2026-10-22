# Runs surface.py on top of Live's real _Framework (the compiled scripts
# shipped inside Live) with a stub `Live` module and a fake c_instance.
#
# Live's scripts are Python 3.11 bytecode, so this needs Python 3.11 and a
# local Live 12 install; otherwise it is skipped. Set MYKONTROL_LIVE_APP to
# point at a different Live.app.
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import sys
import time
import types
import unittest

from fakes import Clock, FakeSong
from MyKontrol import protocol as p
from screen import Screen

LIVE_APP = os.environ.get("MYKONTROL_LIVE_APP", "/Applications/Ableton Live 12 Suite.app")
RESOURCES = os.path.join(LIVE_APP, "Contents", "App-Resources")
SCRIPTS = os.path.join(RESOURCES, "MIDI Remote Scripts")
FUTURE = os.path.join(RESOURCES, "Python", "site-packages", "future-0.18.3")

AVAILABLE = sys.version_info[:2] == (3, 11) and os.path.isdir(os.path.join(SCRIPTS, "_Framework"))


class _StubMeta(type):
    def __getattr__(cls, name):
        if name.startswith("__"):
            raise AttributeError(name)
        value = _StubMeta(name, (object,), {})
        setattr(cls, name, value)
        return value


class _StubModule(types.ModuleType):
    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        value = _StubMeta(name, (object,), {})
        setattr(self, name, value)
        return value


class _MidiMap(object, metaclass=_StubMeta):
    forwarded = []

    @classmethod
    def forward_midi_note(cls, handle, midi_map, channel, note):
        cls.forwarded.append(("note", channel, note))
        return True

    @classmethod
    def forward_midi_cc(cls, handle, midi_map, channel, cc):
        cls.forwarded.append(("cc", channel, cc))
        return True


def _install_live_stub():
    live = _StubModule("Live")
    live.MidiMap = _MidiMap
    sys.modules["Live"] = live
    for path in (SCRIPTS, FUTURE):
        if path not in sys.path:
            sys.path.insert(0, path)


class Listenable(object):
    """Adds Live's add_x_listener / remove_x_listener / x_has_listener."""

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        listeners = self.__dict__.setdefault("_listeners", {})
        for prefix, suffix in (("add_", "_listener"), ("remove_", "_listener"),
                               ("", "_has_listener")):
            if name.startswith(prefix) and name.endswith(suffix):
                prop = name[len(prefix):-len(suffix)]
                cbs = listeners.setdefault(prop, [])
                if prefix == "add_":
                    return cbs.append
                if prefix == "remove_":
                    return cbs.remove
                return lambda cb: cb in cbs
        raise AttributeError(name)


class LiveSong(FakeSong, Listenable):
    pass


class CInstance(object):

    def __init__(self, song):
        self._song = song
        self.sent = []
        self.log = []
        self.highlights = []

    def song(self):
        return self._song

    def send_midi(self, midi_bytes):
        self.sent.append(tuple(midi_bytes))

    def log_message(self, msg):
        self.log.append(msg)

    def show_message(self, msg):
        pass

    def handle(self):
        return 1

    def request_rebuild_midi_map(self):
        pass

    def update_locks(self):
        pass

    def set_session_highlight(self, *args):
        self.highlights.append(args)

    def instance_identifier(self):
        return 0


@unittest.skipUnless(AVAILABLE, "needs Python 3.11 and a local Ableton Live 12 install")
class LiveGlue(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        _install_live_stub()

    def make(self):
        import MyKontrol
        song = LiveSong()
        song.view.__class__ = type("LiveSongView", (song.view.__class__, Listenable), {})
        c = CInstance(song)
        surface = MyKontrol.create_instance(c)
        surface._engine._clock = self.clock = Clock(time.monotonic())   # deterministic
        return surface, c, song

    def test_capabilities(self):
        import MyKontrol
        from ableton.v2.control_surface import capabilities as caps
        cap = MyKontrol.get_capabilities()
        ident = cap[caps.CONTROLLER_ID_KEY]
        self.assertEqual(ident[caps.VENDORID], 0x17CC)
        self.assertEqual(ident[caps.PRODUCTIDS], [0x1860])
        ports = cap[caps.PORTS_KEY]
        self.assertEqual([port[caps.DIRECTIONKEY] for port in ports], ["in", "in", "out", "out"])
        self.assertIn(caps.SCRIPT, ports[1][caps.PROPSKEY])
        self.assertIn(caps.SCRIPT, ports[3][caps.PROPSKEY])
        self.assertNotIn(caps.SCRIPT, ports[0][caps.PROPSKEY])

    def test_lifecycle_through_real_framework(self):
        surface, c, song = self.make()
        self.assertEqual(c.sent, [p.hello()])
        self.assertTrue(song.current_song_time_has_listener(surface._on_song_time))

        _MidiMap.forwarded[:] = []
        surface.build_midi_map(object())
        self.assertIn(("note", 15, p.CTL_PLAY), _MidiMap.forwarded)
        self.assertIn(("note", 15, 0x47), _MidiMap.forwarded)
        self.assertIn(("cc", 15, 0x17), _MidiMap.forwarded)
        self.assertIn(("cc", 15, 0x30), _MidiMap.forwarded)

        surface.receive_midi(p.hi())
        surface.receive_midi(p.ident())
        for _ in range(5):
            self.clock.advance(0.1)
            surface.update_display()
        screen = Screen()
        for m in c.sent:
            screen.feed(m)
        self.assertEqual(screen.text(p.TEXT_TRACK), "Track 1")
        self.assertEqual(screen.text(p.TEXT_KNOB_LABEL), "Dev 1.1 P1")

        surface.receive_midi(p.button(p.CTL_PLAY, True))
        self.assertTrue(song.is_playing)
        surface.receive_midi(p.knob_turn(0, 10))
        self.assertAlmostEqual(song.visible_tracks[0].devices[0].parameters[1].value, 0.55)
        song.view.selected_track = song.visible_tracks[1]
        surface._on_selected_track_changed()     # what _Framework's listener calls
        screen = Screen()
        for m in c.sent:
            screen.feed(m)
        self.assertEqual(screen.text(p.TEXT_TRACK), "Track 2")
        surface.receive_midi(p.view(p.VIEW_CLIPS))
        self.clock.advance(0.1)
        surface.update_display()
        self.assertEqual(c.highlights[-1], (0, 0, 8, 4, False))
        song.view.selected_scene = song.scenes[5]
        surface._on_selected_scene_changed()
        self.assertEqual(c.highlights[-1], (0, 2, 8, 4, False))
        song._listeners["current_song_time"][0]()   # Live notifying a song time change

        errors = [m for m in c.log if "error" in m]
        self.assertEqual(errors, [])

        surface.disconnect()
        self.assertEqual(c.sent[-1], p.bye())
        self.assertFalse(song.current_song_time_has_listener(surface._on_song_time))
        self.assertEqual(c.highlights[-1], (-1, -1, -1, -1, False))


if __name__ == "__main__":
    unittest.main()
