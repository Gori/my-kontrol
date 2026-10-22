# Routes keyboard input to the global buttons and the active view, and
# renders the active view.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later

from . import protocol as p
from .transport import TransportComponent
from .device_view import DeviceView
from .mixer_view import MixerView
from .sends_view import SendsView
from .clip_view import ClipView
from .liveutil import DefaultBanks


class Controller(object):

    def __init__(self, song, display, banks=None, highlight=None):
        self._song = song
        self._display = display
        self.shift = False
        self.transport = TransportComponent(song, display)
        self.views = {
            p.VIEW_DEVICE: DeviceView(song, display, banks or DefaultBanks()),
            p.VIEW_VOLUME: MixerView(song, display),
            p.VIEW_PAN: MixerView(song, display, pan=True),
            p.VIEW_SENDS: SendsView(song, display),
            p.VIEW_CLIPS: ClipView(song, display, highlight),
        }
        self.view_id = p.VIEW_DEVICE
        self.view.enter()

    @property
    def view(self):
        return self.views[self.view_id]

    def set_view(self, view_id):
        if view_id not in self.views:
            return False
        if view_id != self.view_id:
            self.view.exit()
            self.view_id = view_id
            self.view.enter()
        return True

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
                self.view.turn(event.knob, event.delta, self.shift)
        elif isinstance(event, p.KnobTouch):
            if 0 <= event.knob < 8:
                self.view.touch(event.knob, event.touched, self.shift)
        elif isinstance(event, p.EncoderTurn):
            self.view.encoder(event.delta, self.shift)

    def render(self):
        self.transport.render()
        self.view.render(self.shift)

    def render_transport(self):
        self.transport.render_transport()

    def shutdown(self):
        self.view.exit()
