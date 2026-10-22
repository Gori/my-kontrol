# Small helpers around Live's object model.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything here is duck-typed against the Live API (Song, Track,
# DeviceParameter, ClipSlot...) and never imports Live, so the unit tests can
# drive it with plain fake objects. Calls that Live may refuse (setting a
# disabled parameter, arming a group track) are wrapped so a refused action
# is a no-op instead of an exception in the script.

from . import protocol as p

# Knob feel. One relative step from the keyboard moves a continuous
# parameter by 1/CONTINUOUS_STEPS of its range; a stepped (quantized)
# parameter moves one value per STEPPED_TICKS steps.
CONTINUOUS_STEPS = 200.0
STEPPED_TICKS = 4


def live_id(obj):
    """A stable identity for a Live object (Python wrappers are recreated)."""
    return getattr(obj, "_live_ptr", None) or id(obj)


def same(a, b):
    return a is not None and b is not None and live_id(a) == live_id(b)


def index_of(items, obj):
    for i, item in enumerate(items):
        if same(item, obj):
            return i
    return -1


def safe_name(obj, default=""):
    try:
        return obj.name if obj is not None else default
    except Exception:
        return default


def attempt(fn, *args):
    """Call a Live API function; False if Live refused."""
    try:
        fn(*args)
        return True
    except Exception:
        return False


def set_attr(obj, name, value):
    try:
        setattr(obj, name, value)
        return True
    except Exception:
        return False


# -- tracks -----------------------------------------------------------------

def all_tracks(song):
    """Tracks reachable from the keyboard: visible tracks, returns, master."""
    return list(song.visible_tracks) + list(song.return_tracks) + [song.master_track]


def selected_track(song):
    return song.view.selected_track


def is_master(song, track):
    return same(track, song.master_track)


def is_return(song, track):
    return index_of(song.return_tracks, track) >= 0


def has_mute(song, track):
    return track is not None and not is_master(song, track)


def can_arm(track):
    try:
        return bool(track is not None and track.can_be_armed)
    except Exception:
        return False


def select_relative_track(song, delta):
    tracks = all_tracks(song)
    if not tracks or not delta:
        return False
    idx = index_of(tracks, selected_track(song))
    new = max(0, min(len(tracks) - 1, (idx if idx >= 0 else 0) + delta))
    if new == idx:
        return False
    return set_attr(song.view, "selected_track", tracks[new])


def neighbour_exists(song, delta):
    tracks = all_tracks(song)
    idx = index_of(tracks, selected_track(song))
    return 0 <= idx + delta < len(tracks)


def toggle_arm(track):
    if not can_arm(track):
        return None
    if set_attr(track, "arm", not track.arm):
        return track.arm
    return None


# -- devices ----------------------------------------------------------------

def selected_device(song):
    track = selected_track(song)
    if track is None:
        return None
    try:
        return track.view.selected_device
    except Exception:
        return None


def select_relative_device(song, delta):
    track = selected_track(song)
    if track is None:
        return False
    devices = list(track.devices)
    if not devices:
        return False
    idx = index_of(devices, selected_device(song))
    if idx < 0:
        new = 0 if delta > 0 else len(devices) - 1
    else:
        new = max(0, min(len(devices) - 1, idx + delta))
        if new == idx:
            return False
    return attempt(song.view.select_device, devices[new])


def device_neighbour_exists(song, delta):
    track = selected_track(song)
    if track is None:
        return False
    devices = list(track.devices)
    idx = index_of(devices, selected_device(song))
    if idx < 0:
        return bool(devices)
    return 0 <= idx + delta < len(devices)


def device_on_parameter(device):
    params = list(device.parameters) if device is not None else []
    return params[0] if params else None


def toggle_device(device):
    """Flip "Device On". Returns the new state, or None if not possible."""
    on = device_on_parameter(device)
    if on is None or not on.is_enabled:
        return None
    new = on.max if on.value <= on.min else on.min
    if set_attr(on, "value", new):
        return on.value > on.min
    return None


class DefaultBanks(object):
    """Parameter banks without Live's bank tables: the parameters after
    "Device On", eight at a time. The script prefers Live's own tables
    (_Generic.Devices) when they are available; see surface.py."""

    def banks(self, device):
        params = list(device.parameters)[1:] if device is not None else []
        out = []
        for i in range(0, len(params), 8):
            bank = params[i:i + 8]
            out.append(bank + [None] * (8 - len(bank)))
        return out

    def names(self, device):
        return ["Bank %d" % (i + 1) for i in range(len(self.banks(device)))]


# -- parameters -------------------------------------------------------------

def parameter_flags(param):
    if param is None:
        return 0
    flags = p.PARAM_MAPPED
    try:
        lo, hi = float(param.min), float(param.max)
        if lo < 0 < hi and abs(lo + hi) <= 1e-6 * (hi - lo):
            flags |= p.PARAM_BIPOLAR
        if param.is_quantized:
            flags |= p.PARAM_STEPPED
    except Exception:
        pass
    return flags


def parameter_position(param):
    """Normalised 0..127 position for the PARAM bar."""
    if param is None:
        return 0
    try:
        lo, hi = float(param.min), float(param.max)
        if hi <= lo:
            return 0
        return p.normalised((float(param.value) - lo) / (hi - lo))
    except Exception:
        return 0


def parameter_text(param):
    if param is None:
        return ""
    try:
        return str(param)
    except Exception:
        return ""


def show_parameter(display, knob, param, label=None):
    if param is None:
        display.empty_knob(knob)
        return
    display.knob(knob,
                 safe_name(param) if label is None else label,
                 parameter_text(param),
                 parameter_position(param),
                 parameter_flags(param))


class Nudger(object):
    """Applies relative knob steps to parameters, accumulating sub-steps of
    stepped parameters per knob."""

    def __init__(self):
        self._acc = {}

    def reset(self):
        self._acc.clear()

    def nudge(self, knob, param, delta):
        if param is None or not delta:
            return False
        try:
            if not param.is_enabled:
                return False
            lo, hi = param.min, param.max
            if param.is_quantized:
                key = (knob, live_id(param))
                acc = self._acc.get(key, 0) + delta
                steps = int(acc / STEPPED_TICKS)          # truncates towards 0
                self._acc[key] = acc - steps * STEPPED_TICKS
                if not steps:
                    return False
                new = max(lo, min(hi, param.value + steps))
            else:
                new = max(lo, min(hi, param.value + delta * (hi - lo) / CONTINUOUS_STEPS))
            if new == param.value:
                return False
            param.value = new
            return True
        except Exception:
            return False


def begin_gesture(param):
    if param is not None and hasattr(param, "begin_gesture"):
        attempt(param.begin_gesture)


def end_gesture(param):
    if param is not None and hasattr(param, "end_gesture"):
        attempt(param.end_gesture)


# -- mixer ------------------------------------------------------------------

def volume(track):
    try:
        return track.mixer_device.volume
    except Exception:
        return None


def panning(track):
    try:
        return track.mixer_device.panning
    except Exception:
        return None


# -- clips ------------------------------------------------------------------

def slot_state(track, slot):
    """GRID state for one clip slot (track or slot None = no track/scene)."""
    if track is None or slot is None:
        return p.SLOT_NONE
    try:
        if slot.has_clip:
            clip = slot.clip
            if clip.is_recording:
                return p.SLOT_RECORDING
            if clip.is_playing:
                return p.SLOT_PLAYING
            if clip.is_triggered:
                return p.SLOT_QUEUED_RECORD if slot.will_record_on_start else p.SLOT_QUEUED
            return p.SLOT_STOPPED
        if slot.is_triggered and slot.will_record_on_start:
            return p.SLOT_QUEUED_RECORD
        if can_arm(track) and track.arm:
            return p.SLOT_ARMED
        return p.SLOT_EMPTY
    except Exception:
        return p.SLOT_EMPTY


def selected_clip(song):
    """The clip Quantize acts on: the detail clip, else the highlighted slot's."""
    view = song.view
    clip = getattr(view, "detail_clip", None)
    if clip is not None:
        return clip
    slot = getattr(view, "highlighted_clip_slot", None)
    if slot is not None and slot.has_clip:
        return slot.clip
    return None


_QUANTIZE_LABELS = {
    "rec_q_quarter": "1/4",
    "rec_q_eight": "1/8",
    "rec_q_eight_triplet": "1/8T",
    "rec_q_eight_eight_triplet": "1/8+T",
    "rec_q_sixtenth": "1/16",           # sic: Live's spelling
    "rec_q_sixtenth_triplet": "1/16T",
    "rec_q_sixtenth_sixtenth_triplet": "1/16+T",
    "rec_q_thirtysecond": "1/32",
}


def quantize_clip(song, clip):
    """Quantize clip to Live's record quantization (1/16 if that is off).
    Returns the grid label, or None if it failed."""
    q = song.midi_recording_quantization
    name = str(q).split(".")[-1]
    if name not in _QUANTIZE_LABELS:
        q = getattr(type(q), "rec_q_sixtenth", 5)
        name = "rec_q_sixtenth"
    if attempt(clip.quantize, q, 1.0):
        return _QUANTIZE_LABELS[name]
    return None
