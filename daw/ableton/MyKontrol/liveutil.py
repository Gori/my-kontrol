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

import re

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


def device_siblings(song, device):
    """The devices next to `device` in its chain (or on the track)."""
    parent = getattr(device, "canonical_parent", None) if device is not None else None
    devices = getattr(parent, "devices", None)
    if devices is None:
        track = selected_track(song)
        devices = track.devices if track is not None else []
    return list(devices)


def select_relative_device(song, delta):
    """Select the previous / next device in the selected device's chain."""
    current = selected_device(song)
    devices = device_siblings(song, current)
    if not devices:
        return False
    idx = index_of(devices, current)
    if idx < 0:
        new = 0 if delta > 0 else len(devices) - 1
    else:
        new = max(0, min(len(devices) - 1, idx + delta))
        if new == idx:
            return False
    return attempt(song.view.select_device, devices[new])


def device_neighbour_exists(song, delta):
    current = selected_device(song)
    devices = device_siblings(song, current)
    idx = index_of(devices, current)
    if idx < 0:
        return bool(devices)
    return 0 <= idx + delta < len(devices)


INSTRUMENT_TYPE = 1    # Live.Device.DeviceType.instrument


def is_instrument(device):
    try:
        kind = device.type
    except Exception:
        return False
    try:
        return int(kind) == INSTRUMENT_TYPE
    except Exception:
        return str(kind).split(".")[-1] == "instrument"


def is_rack(device):
    try:
        return bool(device.can_have_chains)
    except Exception:
        return False


def _first_instrument(devices):
    for device in devices:
        if is_instrument(device):
            return device
    return None


def _rack_chains(rack):
    """Chains of a rack, the one Live shows as selected first."""
    chains = list(getattr(rack, "chains", ()) or ())
    view = getattr(rack, "view", None)
    preferred = None
    try:
        if getattr(rack, "can_have_drum_pads", False):
            pad = view.selected_drum_pad
            pad_chains = list(pad.chains) if pad is not None else []
            preferred = pad_chains[0] if pad_chains else None
        else:
            preferred = view.selected_chain
    except Exception:
        preferred = None
    if preferred is not None:
        chains = [preferred] + [c for c in chains if not same(c, preferred)]
    return chains


def rack_instrument(rack, depth=0):
    """The instrument playing inside a rack, searching nested racks."""
    if depth > 8:
        return None
    for chain in _rack_chains(rack):
        inner = _first_instrument(getattr(chain, "devices", ()))
        if inner is None:
            continue
        if is_rack(inner):
            found = rack_instrument(inner, depth + 1)
            if found is not None:
                return found
            continue
        return inner
    return None


def drum_rack(track):
    """The track's first instrument if it is a Drum Rack, else None."""
    rack, _ = track_instrument(track)
    return rack if getattr(rack, "can_have_drum_pads", False) else None


def track_instrument(track):
    """(rack, instrument) for the track's first instrument. rack is set when
    that first instrument is an Instrument or Drum Rack; instrument is the
    device playing inside it (or the plain instrument). Either may be None."""
    if track is None:
        return None, None
    try:
        first = _first_instrument(track.devices)
    except Exception:
        return None, None
    if first is None:
        return None, None
    if is_rack(first):
        return first, rack_instrument(first)
    return None, first


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


def parameter_pos(param):
    """Normalised u14 position (v2 VALUE)."""
    if param is None:
        return 0
    try:
        lo, hi = float(param.min), float(param.max)
        if hi <= lo:
            return 0
        return p.position((float(param.value) - lo) / (hi - lo))
    except Exception:
        return 0


def slot_info(param):
    """(kind, flags, steps, default pos) for a v2 SLOT.

    A quantized parameter with two values is a toggle, one with value
    names an enum; everything else (including quantized numbers such as
    a transpose in semitones) is continuous."""
    kind, flags, steps, default = p.KIND_CONTINUOUS, 0, 0, 0
    try:
        lo, hi = float(param.min), float(param.max)
    except Exception:
        return kind, flags, steps, default
    if lo < 0 < hi and abs(lo + hi) <= 1e-6 * (hi - lo):
        flags |= p.SLOT_BIPOLAR
    try:
        if param.is_quantized:
            items = list(getattr(param, "value_items", None) or ())
            count = len(items) if items else int(round(hi - lo)) + 1
            if count == 2:
                kind = p.KIND_TOGGLE
            elif items:
                kind, steps = p.KIND_ENUM, min(127, len(items))
    except Exception:
        pass
    try:
        # Live only has a default for parameters that are not quantized.
        value = float(param.default_value)
        if hi > lo:
            flags |= p.SLOT_HAS_DEFAULT
            default = p.position((value - lo) / (hi - lo))
    except Exception:
        pass
    return kind, flags, steps, default


def reset_to_default(param):
    try:
        if param is not None and param.is_enabled:
            param.value = param.default_value
            return True
    except Exception:
        pass
    return False


def colour(obj):
    """Live's colour (0xRRGGBB) as a protocol colour: each byte >> 1, black
    as 01 01 01, and 00 00 00 when the object has no colour."""
    try:
        value = obj.color
    except Exception:
        return p.NO_COLOUR
    if value is None:
        return p.NO_COLOUR
    try:
        value = int(value)
    except Exception:
        return p.NO_COLOUR
    rgb = (((value >> 16) & 0xFF) >> 1, ((value >> 8) & 0xFF) >> 1, (value & 0xFF) >> 1)
    return rgb if rgb != (0, 0, 0) else p.BLACK


def parameter_text(param):
    """Live's own value text, e.g. "-6.0 dB", "440 Hz", "12L"."""
    if param is None:
        return ""
    try:
        return param.str_for_value(param.value)
    except Exception:
        pass
    try:
        return str(param)
    except Exception:
        return ""


def show_parameter(display, knob, param, label=None, colour=p.NO_COLOUR, group=""):
    if param is None:
        display.empty_knob(knob)
        return
    name = safe_name(param) if label is None else label
    if display.v2:
        kind, flags, steps, default = slot_info(param)
        display.slot(knob, kind, flags, steps, default, colour, name, group,
                     parameter_pos(param), parameter_text(param))
    else:
        display.knob(knob, name, parameter_text(param), parameter_position(param),
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


def sends(track):
    """The track's send parameters (A, B, ...); empty for the master."""
    try:
        return list(track.mixer_device.sends)
    except Exception:
        return []


# Live's output meters (Track.output_meter_left / _right) run from 0.0 to
# 1.0 on the scale Live draws its own meters with, which Live does not
# document. We take it to follow the volume fader's taper: 0.85 is 0 dBFS
# and, below that, the level in dB falls about linearly with the meter
# value, to roughly -70 dB at 0. That is an approximation, close around
# the top of the range and loose near the bottom. METER (v2) wants
# 0 = silence and 1..127 linear in dB from -60 dBFS to 0 dBFS and above.
METER_ZERO_DB = 0.85
METER_DB_AT_ZERO = -70.0
METER_FLOOR_DB = -60.0


def meter_level(value):
    """Live meter value (0..1) -> METER level byte."""
    try:
        value = float(value)
    except Exception:
        return 0
    if value <= 0.0:
        return 0
    db = (value - METER_ZERO_DB) / METER_ZERO_DB * -METER_DB_AT_ZERO
    if db <= METER_FLOOR_DB:
        return 0
    return max(1, min(127, 1 + int(round(126 * (db - METER_FLOOR_DB) / -METER_FLOOR_DB))))


def track_levels(track):
    """(left, right) output meter of a track, 0..1 each."""
    out = []
    for name in ("output_meter_left", "output_meter_right"):
        try:
            out.append(float(getattr(track, name)))
        except Exception:
            out.append(0.0)
    return tuple(out)


# -- song key ---------------------------------------------------------------

def scale_state(song):
    """(state, root, mask, name) for SCALE. Live 12 has a song key (root
    note and scale) and a Scale Mode switch; without them, or with the
    switch off, no key is set."""
    try:
        if not song.scale_mode:
            return p.SCALE_NONE, 0, 1, ""
        root = int(song.root_note) % 12
        mask = 1
        for interval in song.scale_intervals:
            mask |= 1 << (int(interval) % 12)
        return p.SCALE_SONG, root, mask, str(song.scale_name or "")
    except Exception:
        return p.SCALE_NONE, 0, 1, ""


# -- drum pads --------------------------------------------------------------

# Most important first: the keyboard keeps only the first key_labels.
ROLE_ORDER = (p.ROLE_KICK, p.ROLE_SNARE, p.ROLE_CLOSED_HAT, p.ROLE_OPEN_HAT,
              p.ROLE_PEDAL_HAT, p.ROLE_CLAP, p.ROLE_RIM, p.ROLE_TOM, p.ROLE_CYMBAL,
              p.ROLE_PERCUSSION, p.ROLE_OTHER)
_NOT_TOMS = ("custom", "bottom", "atom")


def key_role(name):
    """Guess a drum role from a pad name ("Kick 808", "Hihat Open"...)."""
    s = str(name or "").lower()
    words = re.findall(r"[a-z]+", s)

    def has(*parts):
        return any(part in s for part in parts)

    def word(*names):
        return any(w in names for w in words)

    def edge(*parts):   # a word starting or ending with one of parts
        return any(w.startswith(part) or w.endswith(part) for w in words for part in parts)

    hat = has("hat") or word("hh", "chh", "ohh", "ch", "oh")
    if edge("rim", "stick"):
        return p.ROLE_RIM
    if has("clap"):
        return p.ROLE_CLAP
    if has("kick") or word("bd", "kik"):
        return p.ROLE_KICK
    if has("snare") or word("sd", "snr"):
        return p.ROLE_SNARE
    if word("ohh", "oh") or (hat and has("open")):
        return p.ROLE_OPEN_HAT
    if has("pedal") or word("phh"):
        return p.ROLE_PEDAL_HAT
    if hat:
        return p.ROLE_CLOSED_HAT
    if edge("tom") and not word(*_NOT_TOMS):
        return p.ROLE_TOM
    if has("cymbal", "crash", "ride", "china", "splash"):
        return p.ROLE_CYMBAL
    if has("perc", "shaker", "conga", "bongo", "tamb"):
        return p.ROLE_PERCUSSION
    return p.ROLE_OTHER


def drum_keys(rack, limit):
    """KEY entries [(note, role, colour, label)] for the pads of a Drum Rack
    that hold a chain, in ROLE_ORDER, then by note; at most `limit`."""
    if rack is None:
        return []
    entries = []
    try:
        pads = list(rack.drum_pads)
    except Exception:
        return []
    for pad in pads:
        try:
            chains = list(pad.chains)
            if not chains:
                continue
            name, note = safe_name(pad), int(pad.note)
        except Exception:
            continue
        role = key_role(name)
        entries.append((ROLE_ORDER.index(role), note, role, colour(chains[0]), name))
    entries.sort(key=lambda e: (e[0], e[1]))
    return [(note, role, rgb, name) for _, note, role, rgb, name in entries[:limit]]


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
