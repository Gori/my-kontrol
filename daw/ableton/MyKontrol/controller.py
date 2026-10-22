# Routes keyboard input to the global buttons and the active view, and
# renders the active view.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# With a v2 keyboard it also owns what is not tied to one view: the MODES
# list and when to suggest a mode (SET_MODE), meters, the song key (SCALE),
# drum pad names (KEY) and closing the list / message screens.

from . import protocol as p
from . import liveutil as lu
from .transport import TransportComponent
from .device_view import InstrumentView, PluginView
from .mixer_view import MixerView
from .sends_view import SendsView
from .clip_view import ClipView
from .browser_view import MODE_BROWSER, BrowserView

# MODES order: the standard modes, then ours.
MODE_ORDER = (p.VIEW_INSTRUMENT, p.VIEW_PLUGIN, p.VIEW_VOLUME, p.VIEW_PAN, p.VIEW_SENDS,
              p.VIEW_CLIPS, MODE_BROWSER)
MIXER_MODES = (p.VIEW_VOLUME, p.VIEW_PAN, p.VIEW_SENDS)
DEVICE_MODES = (p.VIEW_INSTRUMENT, p.VIEW_PLUGIN)
KEY_RESCAN_TICKS = 10      # re-read drum pad names about once a second


class Controller(object):

    def __init__(self, song, display, banks=None, highlight=None, browser=None):
        self._song = song
        self._display = display
        self.shift = False
        self.transport = TransportComponent(song, display)
        self.views = {
            p.VIEW_INSTRUMENT: InstrumentView(song, display, banks),
            p.VIEW_PLUGIN: PluginView(song, display, banks),
            p.VIEW_VOLUME: MixerView(song, display),
            p.VIEW_PAN: MixerView(song, display, pan=True),
            p.VIEW_SENDS: SendsView(song, display),
            p.VIEW_CLIPS: ClipView(song, display, highlight),
        }
        if browser is not None:
            self.views[MODE_BROWSER] = BrowserView(song, display, browser)
        for view_id, view in self.views.items():
            view.mode = view_id
        self.view_id = p.VIEW_INSTRUMENT     # the keyboard's first knob mode
        self._device_seen = None    # (track, device) last seen selected
        self._keys_source = None
        self._key_entries = []
        self._keys_age = 0
        self.view.enter()

    @property
    def view(self):
        return self.views[self.view_id]

    def set_view(self, view_id):
        if view_id not in self.views:
            return False
        if view_id == MODE_BROWSER and not self._display.v2:
            return False
        if view_id != self.view_id:
            self.view.exit()
            self.view_id = view_id
            self.view.enter()
            self._display.hot_knob = None
            self._display.forget_meter(p.METER_CHANNEL)
        return True

    def connected(self):
        """A keyboard just answered the handshake."""
        self.shift = False      # a fresh device has no key held
        self._device_seen = self._device_key()
        self._keys_source = None

    # -- modes (v2) ---------------------------------------------------------

    def modes(self):
        """MODES entries [(id, flags, name)]."""
        song = self._song
        rack, instrument = lu.track_instrument(lu.selected_track(song))
        enabled = {
            p.VIEW_INSTRUMENT: rack is not None or instrument is not None,
            p.VIEW_PLUGIN: lu.selected_device(song) is not None,
            p.VIEW_SENDS: len(list(song.return_tracks)) > 0,
        }
        browser = self.views.get(MODE_BROWSER)
        if browser is not None:
            enabled[MODE_BROWSER] = browser.available()
        return [(mode, p.MODE_ENABLED if enabled.get(mode, True) else 0,
                 "Browser" if mode == MODE_BROWSER else "")
                for mode in MODE_ORDER if mode in self.views]

    def _device_key(self):
        track = lu.selected_track(self._song)
        device = lu.selected_device(self._song)
        return (lu.live_id(track) if track is not None else None,
                lu.live_id(device) if device is not None else None)

    def poll_device_selection(self):
        """The mode to suggest (SET_MODE) when the user selected another
        device on the same track in Live, else None. Selections made by our
        own controls are recorded in handle() and never suggest a mode."""
        key, seen = self._device_key(), self._device_seen
        self._device_seen = key
        if seen is None or key == seen or key[0] != seen[0] or key[1] is None:
            return None
        if self.view_id == MODE_BROWSER:
            return None     # loading from the browser selects the new device
        rack, instrument = lu.track_instrument(lu.selected_track(self._song))
        device = lu.selected_device(self._song)
        mode = (p.VIEW_INSTRUMENT if lu.same(device, rack) or lu.same(device, instrument)
                else p.VIEW_PLUGIN)
        return mode if mode != self.view_id else None

    # -- input --------------------------------------------------------------

    def handle(self, event):
        """Apply one control event (protocol.Button / KnobTurn / ...)."""
        if isinstance(event, p.Button):
            if event.control == p.CTL_SHIFT:
                self.shift = event.pressed
            elif event.pressed:
                if not self.transport.press(event.control, self.shift):
                    self.view.press(event.control, self.shift)
        elif isinstance(event, p.KnobTurn):
            if 0 <= event.knob < 8:
                self._display.hot_knob = event.knob
                self.view.turn(event.knob, event.delta, self.shift)
        elif isinstance(event, p.KnobTouch):
            if 0 <= event.knob < 8:
                if event.touched:
                    self._display.hot_knob = event.knob
                self.view.touch(event.knob, event.touched, self.shift)
        elif isinstance(event, p.EncoderTurn):
            self.view.encoder(event.delta, self.shift)
        self._device_seen = self._device_key()

    # -- output -------------------------------------------------------------

    def render(self):
        self.transport.render()
        self.view.render(self.shift)
        d = self._display
        if d.v2:
            if not self.view.owns_list:
                d.close_list()
            d.close_message()
            d.scale(*lu.scale_state(self._song))
            self._render_keys(rescan=False)

    def render_periodic(self, now):
        """Once per tick (v2): meters, and drum pad names now and then."""
        if not self._display.v2:
            return
        self._render_keys(rescan=True)
        d = self._display
        if self.view_id in MIXER_MODES:
            tracks = list(self.view.meter_tracks() or ())
            d.meter(p.METER_SLOTS, [lu.meter_level(max(lu.track_levels(t))) if t is not None
                                    else 0 for t in tracks], now)
        elif self.view_id in DEVICE_MODES:
            track = lu.selected_track(self._song)
            d.meter(p.METER_CHANNEL, [lu.meter_level(v) for v in lu.track_levels(track)], now)

    def _render_keys(self, rescan):
        track = lu.selected_track(self._song)
        rack = lu.drum_rack(track)
        source = (lu.live_id(track) if track is not None else None,
                  lu.live_id(rack) if rack is not None else None)
        if source != self._keys_source or (rescan and self._keys_age >= KEY_RESCAN_TICKS):
            self._keys_source = source
            self._keys_age = 0
            self._key_entries = lu.drum_keys(rack, self._display.key_limit())
        elif rescan:
            self._keys_age += 1
        self._display.keys(self._key_entries)

    def render_transport(self):
        self.transport.render_transport()

    def shutdown(self):
        self.view.exit()
