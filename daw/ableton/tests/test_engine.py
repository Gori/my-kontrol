# End-to-end tests: device bytes in -> Engine (with a fake Live song) ->
# host bytes out, checked through the simulator's Screen model.
# SPDX-License-Identifier: GPL-3.0-or-later

import unittest

from fakes import Clock, FakeClip, FakeSong, Wire
from MyKontrol import protocol as p
from MyKontrol.engine import Engine
from screen import Screen


class Harness(object):

    def __init__(self, **song_kw):
        self.clock = Clock()
        self.wire = Wire(self.clock)
        self.song = FakeSong(**song_kw)
        self.highlights = []
        self.engine = Engine(self.song, self.wire, clock=self.clock,
                             highlight=lambda *a: self.highlights.append(a))
        self.screen = Screen()
        self._seen = 0

    def sync(self):
        new = self.wire.messages()[self._seen:]
        self._seen += len(new)
        for m in new:
            self.screen.feed(m)
        return new

    def connect(self):
        self.engine.start()
        self.engine.receive(p.hi())
        self.engine.receive(p.ident(caps=0x0F))
        self.settle()

    def settle(self, ticks=10):
        for _ in range(ticks):
            self.clock.advance(0.1)
            self.engine.tick()
        return self.sync()

    def send(self, message):
        self.engine.receive(message)
        return self.sync()

    def press(self, control, shift=False):
        if shift:
            self.engine.receive(p.button(p.CTL_SHIFT, True))
        self.engine.receive(p.button(control, True))
        self.engine.receive(p.button(control, False))
        if shift:
            self.engine.receive(p.button(p.CTL_SHIFT, False))
        return self.sync()

    def view(self, view_id):
        self.engine.receive(p.view(view_id))
        return self.settle()

    def text(self, field):
        return self.screen.text(field)


def sysex_cmds(messages):
    return [m[4] for m in messages if m[0] == 0xF0]


class Connection(unittest.TestCase):

    def test_hello_on_start_and_on_hi(self):
        h = Harness()
        h.engine.start()
        self.assertEqual(h.sync(), [p.hello()])
        h.send(p.hi())
        self.assertEqual(h.screen.hello, 2)

    def test_nothing_but_hello_before_ident(self):
        h = Harness()
        h.engine.start()
        h.engine.receive(p.hi())
        h.engine.receive(p.knob_turn(0, 5))
        h.engine.receive(p.button(p.CTL_PLAY, True))
        h.settle()
        self.assertEqual(set(sysex_cmds(h.wire.messages())), {p.CMD_HELLO})
        self.assertEqual(h.song.calls, [])

    def test_full_state_after_ident(self):
        h = Harness()
        h.connect()
        self.assertEqual(h.text(p.TEXT_TRACK), "Track 1")
        self.assertEqual(h.text(p.TEXT_DEVICE), "Dev 1.1")
        self.assertEqual(h.text(p.TEXT_BANK), "Bank 1")
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Bank 1/1")
        for k in range(8):
            self.assertEqual(h.text(p.TEXT_KNOB_LABEL + k), "Dev 1.1 P%d" % (k + 1))
            self.assertEqual(h.text(p.TEXT_KNOB_VALUE + k), "0.50")
            self.assertEqual(h.screen.params[k], (64, p.PARAM_MAPPED))
        self.assertIsNotNone(h.screen.transport)
        self.assertEqual(h.screen.lights[p.CTL_PLAY], p.LIGHT_DIM)

    def test_rate_limit_and_no_spam_when_idle(self):
        h = Harness()
        h.connect()
        times = [t for t, _ in h.wire.sent]
        for t in times:
            in_window = [u for u in times if t <= u < t + 0.1 - 1e-9]
            self.assertLessEqual(len(in_window), 20)
        h.sync()
        h.wire.clear()
        h._seen = 0
        h.settle(ticks=30)       # 3 s with nothing changing
        self.assertEqual(set(sysex_cmds(h.wire.messages())), {p.CMD_PING})
        # At least one message per second keeps the device in host mode.
        self.assertGreaterEqual(len(h.wire.messages()), 3)

    def test_rate_limit_under_load(self):
        h = Harness(n_tracks=12, n_scenes=8)
        h.connect()
        # Knob storm across views with ticks in between, 7 ms apart.
        for i in range(600):
            h.clock.advance(0.007)
            if i % 14 == 0:
                h.engine.tick()
            if i % 100 == 0:
                h.engine.receive(p.view((i // 100) % 3))
            h.engine.receive(p.knob_turn(i % 8, 3 if i % 2 else -2))
        sent = h.wire.sent
        for i, (t, _) in enumerate(sent):
            window = [m for u, m in sent[i:] if u < t + 0.1 - 1e-9]
            self.assertLessEqual(len(window), 20)
            self.assertLessEqual(sum(len(m) for m in window), 960)

    def test_device_owned_lights_are_never_sent(self):
        h = Harness()
        h.connect()
        h.view(p.VIEW_CLIPS)
        h.view(p.VIEW_MIXER)
        for c in p.DEVICE_OWNED_CONTROLS:
            self.assertNotIn(c, h.screen.lights)

    def test_refresh_resends_everything(self):
        h = Harness()
        h.connect()
        h.screen.reset()
        h.send(p.refresh())
        h.settle()
        self.assertEqual(h.text(p.TEXT_TRACK), "Track 1")
        self.assertEqual(len(h.screen.params), 8)

    def test_reconnect_after_device_reset(self):
        h = Harness()
        h.connect()
        h.send(p.hi())
        self.assertFalse(h.engine.connected)
        h.screen.reset()
        h.send(p.ident())
        h.settle()
        self.assertEqual(h.text(p.TEXT_TRACK), "Track 1")

    def test_ident_with_view_byte_starts_in_that_view(self):
        h = Harness()
        h.engine.start()
        h.engine.receive(p.hi())
        h.engine.receive(p.ident(view=p.VIEW_MIXER))
        h.settle()
        self.assertEqual(h.text(p.TEXT_BANK), "Volume")

    def test_bye_on_stop(self):
        h = Harness()
        h.connect()
        h.engine.stop()
        self.assertEqual(h.sync()[-1], p.bye())


class DeviceView(unittest.TestCase):

    def test_knob_changes_parameter_and_sends_only_that(self):
        h = Harness()
        h.connect()
        param = h.song.visible_tracks[0].devices[0].parameters[1]
        out = h.send(p.knob_turn(0, 20))
        self.assertAlmostEqual(param.value, 0.6)
        self.assertEqual(sorted(sysex_cmds(out)), [p.CMD_TEXT, p.CMD_PARAM])
        self.assertEqual(h.text(p.TEXT_KNOB_VALUE), "0.60")

    def test_banks_and_devices(self):
        h = Harness(params_per_device=20)
        h.connect()
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Bank 1/3")
        h.press(p.CTL_PRESET_DOWN)
        h.settle()
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Bank 2/3")
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL), "Dev 1.1 P9")
        h.press(p.CTL_PRESET_DOWN)
        h.settle()
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL + 3), "Dev 1.1 P20")
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL + 4), "")
        self.assertEqual(h.screen.params[4], (0, 0))
        # Shift+Preset down / 4D down: next device; its bank starts at 1.
        h.press(p.CTL_PRESET_DOWN, shift=True)
        h.settle()
        self.assertEqual(h.text(p.TEXT_DEVICE), "Dev 1.2")
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Bank 1/3")
        # Back to the first device: its bank was remembered.
        h.press(p.CTL_NAV_UP)
        h.settle()
        self.assertEqual(h.text(p.TEXT_DEVICE), "Dev 1.1")
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Bank 3/3")

    def test_track_selection_and_device_toggle(self):
        h = Harness()
        h.connect()
        h.send(p.encoder_turn(1))
        h.settle()
        self.assertEqual(h.text(p.TEXT_TRACK), "Track 2")
        h.press(p.CTL_NAV_LEFT)
        h.settle()
        self.assertEqual(h.text(p.TEXT_TRACK), "Track 1")
        h.press(p.CTL_NAV_PUSH)
        self.assertEqual(h.song.visible_tracks[0].devices[0].parameters[0].value, 0)
        self.assertEqual(h.screen.notifications[-1], ("Dev 1.1", "Off"))

    def test_stepped_parameter_needs_several_steps(self):
        h = Harness()
        h.connect()
        param = h.song.visible_tracks[0].devices[0].parameters[1]
        param.is_quantized, param.min, param.max, param.value = True, 0, 5, 0
        for _ in range(3):
            h.send(p.knob_turn(0, 1))
        self.assertEqual(param.value, 0)
        h.send(p.knob_turn(0, 1))
        self.assertEqual(param.value, 1)

    def test_touch_brackets_a_gesture(self):
        h = Harness()
        h.connect()
        param = h.song.visible_tracks[0].devices[0].parameters[2]
        h.send(p.knob_touch(1, True))
        h.send(p.knob_touch(1, False))
        self.assertEqual(param.gestures, ["begin", "end"])


class MixerView(unittest.TestCase):

    def test_volume_and_shift_pan(self):
        h = Harness(n_tracks=10, n_returns=2)
        h.connect()
        h.view(p.VIEW_MIXER)
        self.assertEqual(h.text(p.TEXT_BANK), "Volume")
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Tracks 1-8")
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL + 2), "Track 3")
        h.send(p.knob_turn(2, -10))
        self.assertAlmostEqual(h.song.visible_tracks[2].mixer_device.volume.value, 0.8)
        h.send(p.button(p.CTL_SHIFT, True))
        h.settle()
        self.assertEqual(h.text(p.TEXT_BANK), "Pan")
        self.assertEqual(h.screen.params[0], (64, p.PARAM_MAPPED | p.PARAM_BIPOLAR))
        h.send(p.knob_turn(0, 10))
        self.assertAlmostEqual(h.song.visible_tracks[0].mixer_device.panning.value, 0.1)
        h.send(p.button(p.CTL_SHIFT, False))

    def test_banking_and_selection_follow(self):
        h = Harness(n_tracks=10, n_returns=2)
        h.connect()
        h.view(p.VIEW_MIXER)
        h.press(p.CTL_NAV_RIGHT)
        h.settle()
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Tracks 9-13")
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL + 2), "Return A")
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL + 4), "Master")
        self.assertEqual(h.text(p.TEXT_KNOB_LABEL + 5), "")
        # Selecting a track in bank 1 brings bank 1 back.
        h.song.view.selected_track = h.song.visible_tracks[1]
        h.settle()
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Tracks 1-8")

    def test_push_arms(self):
        h = Harness()
        h.connect()
        h.view(p.VIEW_MIXER)
        h.press(p.CTL_NAV_PUSH)
        self.assertTrue(h.song.visible_tracks[0].arm)


class ClipView(unittest.TestCase):

    def test_grid_states(self):
        h = Harness(n_tracks=3, n_scenes=6)
        t = h.song.visible_tracks
        t[0].clip_slots[0].clip = FakeClip(playing=True)
        t[1].clip_slots[0].clip = FakeClip()
        t[2].clip_slots[1].clip = FakeClip(playing=True, recording=True)
        t[0].clip_slots[1].clip = FakeClip(triggered=True)
        t[1].arm = True
        h.connect()
        h.view(p.VIEW_CLIPS)
        g = h.screen.grid
        self.assertEqual((g.cols, g.rows, g.cursor_col, g.cursor_row), (8, 4, 0, 0))
        row0 = g.states[0:8]
        row1 = g.states[8:16]
        self.assertEqual(row0[:4], (p.SLOT_PLAYING, p.SLOT_STOPPED, p.SLOT_EMPTY, p.SLOT_NONE))
        self.assertEqual(row1[:3], (p.SLOT_QUEUED, p.SLOT_ARMED, p.SLOT_RECORDING))
        self.assertEqual(h.text(p.TEXT_COLUMN + 1), "Track 2")
        self.assertEqual(h.text(p.TEXT_COLUMN + 3), "")
        self.assertEqual(h.text(p.TEXT_ROW), "Scene 1")
        self.assertEqual(h.text(p.TEXT_ROW + 1), "2")         # unnamed scene
        self.assertEqual(h.highlights[-1], (0, 0, 8, 4, False))

    def test_cursor_scrolls_past_edge_and_launches(self):
        h = Harness(n_tracks=3, n_scenes=6)
        h.connect()
        h.view(p.VIEW_CLIPS)
        for _ in range(5):
            h.press(p.CTL_NAV_DOWN)
        h.settle()
        g = h.screen.grid
        self.assertEqual((g.cursor_col, g.cursor_row), (0, 3))
        self.assertEqual(h.text(p.TEXT_ROW), "Scene 3")        # window scrolled by 2
        self.assertEqual(h.highlights[-1], (0, 2, 8, 4, False))
        self.assertIs(h.song.view.selected_scene, h.song.scenes[5])
        h.press(p.CTL_NAV_RIGHT)
        h.press(p.CTL_NAV_PUSH)
        self.assertEqual(h.song.visible_tracks[1].clip_slots[5].fired, 1)
        h.press(p.CTL_NAV_PUSH, shift=True)
        self.assertEqual(h.song.scenes[5].fired, 1)

    def test_encoder_and_preset_paging(self):
        h = Harness(n_tracks=2, n_scenes=12)
        h.connect()
        h.view(p.VIEW_CLIPS)
        h.press(p.CTL_PRESET_DOWN)
        h.settle()
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Tracks 1-2 Scenes 5-8")
        h.send(p.encoder_turn(-1))
        h.settle()
        self.assertEqual(h.text(p.TEXT_SUBTITLE), "Tracks 1-2 Scenes 4-7")

    def test_leaving_hides_session_ring(self):
        h = Harness()
        h.connect()
        h.view(p.VIEW_CLIPS)
        h.view(p.VIEW_DEVICE)
        self.assertEqual(h.highlights[-1], (-1, -1, -1, -1, False))


class Transport(unittest.TestCase):

    def test_buttons(self):
        h = Harness()
        h.connect()
        s = h.song
        h.press(p.CTL_PLAY)
        self.assertTrue(s.is_playing)
        h.press(p.CTL_PLAY)
        self.assertFalse(s.is_playing)
        h.press(p.CTL_PLAY, shift=True)
        self.assertEqual(s.calls[-1], "continue_playing")
        h.press(p.CTL_STOP, shift=True)
        self.assertEqual(s.calls[-1], "stop_all_clips")
        h.press(p.CTL_REC)
        h.press(p.CTL_REC, shift=True)
        self.assertTrue(s.record_mode and s.session_record)
        h.press(p.CTL_LOOP)
        h.press(p.CTL_METRO)
        self.assertTrue(s.loop and s.metronome)
        h.press(p.CTL_TEMPO)
        self.assertEqual(s.calls[-1], "tap_tempo")
        h.press(p.CTL_UNDO)
        self.assertEqual(s.calls[-1], "undo")
        s.can_redo = True
        h.press(p.CTL_UNDO, shift=True)
        self.assertEqual(s.calls[-1], "redo")
        h.press(p.CTL_MUTE)
        h.press(p.CTL_SOLO)
        h.press(p.CTL_SOLO, shift=True)
        t = s.visible_tracks[0]
        self.assertTrue(t.mute and t.solo and t.arm)
        h.settle()
        self.assertEqual(h.screen.lights[p.CTL_REC], p.LIGHT_ON)
        self.assertEqual(h.screen.lights[p.CTL_MUTE], p.LIGHT_ON)
        flags = h.screen.transport.flags
        self.assertTrue(flags & p.TRANSPORT_LOOP and flags & p.TRANSPORT_METRONOME)

    def test_quantize_and_capture(self):
        h = Harness()
        h.connect()
        h.press(p.CTL_QUANTIZE)
        self.assertEqual(h.screen.notifications[-1], ("Quantize", "No clip"))
        clip = FakeClip()
        h.song.view.detail_clip = clip
        h.press(p.CTL_QUANTIZE)
        self.assertEqual(h.screen.notifications[-1], ("Quantize", "1/16"))
        self.assertEqual(len(clip.quantized), 1)
        h.song.can_capture_midi = True
        h.settle()
        self.assertEqual(h.screen.lights[p.CTL_IDEAS], p.LIGHT_ON)
        self.assertTrue(h.screen.transport.flags & p.TRANSPORT_CAPTURE)
        h.press(p.CTL_IDEAS)
        self.assertEqual(h.song.calls[-1], "capture_midi")

    def test_beat_is_sent_immediately(self):
        h = Harness()
        h.connect()
        self.assertEqual(h.screen.transport.beat, p.BEAT_STOPPED)
        h.song.is_playing = True
        for beat in (1, 2, 3):
            h.song.beats = beat
            h.engine.song_time_changed()
            h.sync()
            self.assertEqual(h.screen.transport.beat, beat - 1)
        self.assertEqual(h.screen.transport.tempo, 120.0)
        self.assertEqual(h.screen.transport.beats_per_bar, 4)


if __name__ == "__main__":
    unittest.main()
