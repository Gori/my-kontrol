# Minimal stand-ins for Live's Song / Track / Device / ClipSlot objects.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Only the attributes the script reads are modelled. Each fake behaves like
# the Live API it replaces for the purposes of the script (for example
# select_device() updates the track's selected device).

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "sim"))


class FakeParam(object):

    def __init__(self, name, value=0.0, min=0.0, max=1.0, quantized=False, unit=""):
        self.name = name
        self.value = value
        self.min = min
        self.max = max
        self.is_quantized = quantized
        self.is_enabled = True
        self.unit = unit
        self.gestures = []

    def __str__(self):
        if self.is_quantized:
            return "On" if self.value else "Off" if self.max == 1 else str(int(self.value))
        return "%.2f%s" % (self.value, self.unit)

    def begin_gesture(self):
        self.gestures.append("begin")

    def end_gesture(self):
        self.gestures.append("end")


INSTRUMENT, AUDIO_EFFECT, MIDI_EFFECT = 1, 2, 4


class FakeDevice(object):

    def __init__(self, name, n_params=8, class_name="FakeDevice", type=AUDIO_EFFECT,
                 param_names=None):
        self.name = name
        self.class_name = class_name
        self.type = type
        self.can_have_chains = False
        self.canonical_parent = None
        names = param_names or ["%s P%d" % (name, i + 1) for i in range(n_params)]
        self.parameters = [FakeParam("Device On", 1, 0, 1, quantized=True)] + [
            FakeParam(n, 0.5) for n in names]


class FakeChain(object):

    def __init__(self, devices):
        self.devices = list(devices)
        for d in self.devices:
            d.canonical_parent = self


class FakeRack(FakeDevice):

    def __init__(self, name, chains, drum=False):
        super(FakeRack, self).__init__(
            name, class_name="DrumGroupDevice" if drum else "InstrumentGroupDevice",
            type=INSTRUMENT, param_names=["Macro %d" % i for i in range(1, 17)])
        self.can_have_chains = True
        self.can_have_drum_pads = drum
        self.chains = list(chains)
        self.view = _Obj(selected_chain=None, selected_drum_pad=None)


class FakeClip(object):

    def __init__(self, playing=False, recording=False, triggered=False):
        self.is_playing = playing
        self.is_recording = recording
        self.is_triggered = triggered
        self.quantized = []

    def quantize(self, grid, amount):
        self.quantized.append((grid, amount))


class FakeClipSlot(object):

    def __init__(self, clip=None):
        self.clip = clip
        self.is_triggered = False
        self.will_record_on_start = False
        self.fired = 0

    @property
    def has_clip(self):
        return self.clip is not None

    def fire(self):
        self.fired += 1


class FakeScene(object):

    def __init__(self, name=""):
        self.name = name
        self.fired = 0

    def fire(self):
        self.fired += 1


class _Obj(object):
    def __init__(self, **k):
        self.__dict__.update(k)


class FakeTrack(object):

    def __init__(self, name, n_scenes=0, devices=(), armable=True):
        self.name = name
        self.devices = list(devices)
        for d in self.devices:
            d.canonical_parent = self
        self.view = _Obj(selected_device=self.devices[0] if self.devices else None)
        self.mute = False
        self.solo = False
        self.arm = False
        self.can_be_armed = armable
        self.mixer_device = _Obj(volume=FakeParam("Volume", 0.85, 0.0, 1.0, unit=" dB"),
                                 panning=FakeParam("Pan", 0.0, -1.0, 1.0),
                                 sends=[])
        self.clip_slots = [FakeClipSlot() for _ in range(n_scenes)]


class FakeQuantization(object):
    """Mimics a Boost.Python enum value: str() is the name, the class has
    the other values as attributes."""

    def __init__(self, name):
        self._name = name

    def __str__(self):
        return self._name


FakeQuantization.rec_q_sixtenth = FakeQuantization("rec_q_sixtenth")
FakeQuantization.rec_q_no_q = FakeQuantization("rec_q_no_q")


class FakeSongView(object):

    def __init__(self, song):
        self._song = song
        self.selected_track = None
        self.selected_scene = None
        self.detail_clip = None
        self.highlighted_clip_slot = None

    def select_device(self, device):
        # Live selects nested devices too; the owning track is the one whose
        # chain (directly or through racks) contains the device.
        def contains(devices):
            for d in devices:
                if d is device or any(contains(c.devices) for c in getattr(d, "chains", ())):
                    return True
            return False
        for track in self._song.all_tracks():
            if contains(track.devices):
                track.view.selected_device = device
                self.selected_track = track


class FakeSong(object):

    def __init__(self, n_tracks=3, n_scenes=6, n_returns=1, devices_per_track=2,
                 params_per_device=8):
        self.scenes = [FakeScene("Scene %d" % (i + 1) if i % 2 == 0 else "")
                       for i in range(n_scenes)]
        self.visible_tracks = [
            FakeTrack("Track %d" % (i + 1), n_scenes,
                      [FakeDevice("Dev %d.%d" % (i + 1, d + 1), params_per_device,
                                  type=INSTRUMENT if d == 0 else AUDIO_EFFECT)
                       for d in range(devices_per_track)])
            for i in range(n_tracks)]
        self.tracks = self.visible_tracks
        self.return_tracks = [FakeTrack("Return %c" % chr(65 + i), armable=False)
                              for i in range(n_returns)]
        self.master_track = FakeTrack("Master", armable=False)
        for track in self.visible_tracks + self.return_tracks:
            track.mixer_device.sends = [FakeParam("Send %c" % chr(65 + i), 0.0, unit=" dB")
                                        for i in range(n_returns)]
        self.view = FakeSongView(self)
        self.view.selected_track = self.visible_tracks[0] if self.visible_tracks else None
        self.view.selected_scene = self.scenes[0] if self.scenes else None
        self.is_playing = False
        self.record_mode = False
        self.session_record = False
        self.loop = False
        self.metronome = False
        self.tempo = 120.0
        self.current_song_time = 0.0
        self.signature_numerator = 4
        self.can_undo = True
        self.can_redo = False
        self.can_capture_midi = False
        self.midi_recording_quantization = FakeQuantization("rec_q_no_q")
        self.beats = 1
        self.calls = []

    def all_tracks(self):
        return self.visible_tracks + self.return_tracks + [self.master_track]

    def get_current_beats_song_time(self):
        return _Obj(bars=1, beats=self.beats, sub_division=1, ticks=0)

    def _call(name):
        def method(self):
            self.calls.append(name)
            if name == "start_playing" or name == "continue_playing":
                self.is_playing = True
            elif name == "stop_playing":
                self.is_playing = False
        method.__name__ = name
        return method

    start_playing = _call("start_playing")
    stop_playing = _call("stop_playing")
    continue_playing = _call("continue_playing")
    stop_all_clips = _call("stop_all_clips")
    tap_tempo = _call("tap_tempo")
    undo = _call("undo")
    redo = _call("redo")
    capture_midi = _call("capture_midi")
    del _call


class Wire(object):
    """Collects what the script sends, with the fake clock time of each."""

    def __init__(self, clock):
        self.clock = clock
        self.sent = []   # (time, bytes)

    def __call__(self, message):
        assert all(0 <= b <= 0xFF for b in message)
        self.sent.append((self.clock(), tuple(message)))

    def messages(self):
        return [m for _, m in self.sent]

    def clear(self):
        del self.sent[:]


class Clock(object):

    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt
