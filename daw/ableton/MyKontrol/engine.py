# The whole script minus Live: wire protocol, session, throttling, views.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# surface.py feeds this class from Live's callbacks:
#
#   receive(midi_bytes)   every message from the DAW port
#   tick()                Live's 100 ms update_display() timer
#   song_time_changed()   Song.current_song_time listener (beat flashes)
#   selection_changed()   selected track / scene listeners (arrow keys in Live)
#   state_changed()       other Live listeners (song key) for a prompt redraw
#   start() / stop()      script load / unload
#
# Handshake: we send a v2 HELLO. A v1 IDENT (older firmware) gives the v1
# behaviour. After a v2 IDENT we send MODES and wait for the VIEW the
# keyboard answers with (at most VIEW_TIMEOUT) before the full state. MODES
# goes out again whenever a mode's availability changes; it is not part of
# the full state. A v2 keyboard needs the full state again only after a
# VIEW that changes the mode (or REFRESH), so the VIEW answering a MODES
# re-send or a refused SET_MODE costs nothing. A v1 keyboard gets it after
# every VIEW, as before.
#
# Keeping this free of Live imports means the unit tests exercise the real
# code path from bytes in to bytes out.

import time

from . import protocol as p
from .controller import Controller
from .display import PRIO_NOTIFY, Display
from .outbox import Outbox
from .session import Session

HOST_NAME = "Ableton Live"
VIEW_TIMEOUT = 0.5
FEATURES = (p.FEATURE_PARAMETERS | p.FEATURE_METERS | p.FEATURE_SCALE | p.FEATURE_KEY_LABELS
            | p.FEATURE_COLOURS | p.FEATURE_GRID)


class Engine(object):

    def __init__(self, song, send, clock=time.monotonic, banks=None, highlight=None,
                 log=None, max_messages=20, max_bytes=960, window=0.1,
                 host_version="", browser=None):
        self._send_raw = send
        self._clock = clock
        self._log = log or (lambda *a: None)
        self.last_send = None
        self.outbox = Outbox(self._send, max_messages=max_messages,
                             max_bytes=max_bytes, window=window)
        self.display = Display(self.outbox)
        features = FEATURES | (p.FEATURE_LISTS if browser is not None else 0)
        self.session = Session(log=self._log,
                               hello=p.hello(p.PROTOCOL_V2, features, HOST_NAME, host_version))
        self.controller = Controller(song, self.display, banks=banks, highlight=highlight,
                                     browser=browser)
        self._modes_sent = None
        self._view_deadline = None   # waiting for the VIEW that answers MODES

    # -- plumbing -----------------------------------------------------------

    def _send(self, message):
        self._send_raw(tuple(message))
        self.last_send = self._clock()

    def _send_all(self, messages):
        # Session messages bypass the queue but count against its rate limit.
        for message in messages:
            self.outbox.account(message, self._clock())
            self._send(message)

    @property
    def connected(self):
        return self.session.connected

    @property
    def v2(self):
        return self.connected and self.display.v2

    # -- lifecycle ----------------------------------------------------------

    def start(self):
        self._disconnected()
        self._send_all(self.session.start())

    def stop(self):
        self.controller.shutdown()
        self._disconnected()
        self._send_all(self.session.stop())

    def _disconnected(self):
        self.display.configure(None)
        self._modes_sent = None
        self._view_deadline = None

    # -- Live callbacks -----------------------------------------------------

    def receive(self, midi_bytes):
        event = p.parse(midi_bytes)
        if event is None:
            return
        if isinstance(event, p.Hi):
            self._disconnected()
            self._send_all(self.session.on_hi(event))
        elif isinstance(event, p.Ident):
            messages, full = self.session.on_ident(event)
            self._send_all(messages)
            if full:
                self.display.configure(event)
                self.controller.connected()
                if event.view is not None:
                    self.controller.set_view(event.view)
                if self.display.v2:
                    self._modes_sent = None
                    self._sync_modes()
                    self._view_deadline = self._clock() + VIEW_TIMEOUT
                else:
                    self.full_refresh()
        elif isinstance(event, p.View):
            waiting = self._view_deadline is not None
            self._view_deadline = None
            same = event.view == self.controller.view_id
            if self.controller.set_view(event.view) and not (same and self.v2) or waiting:
                self.full_refresh()
        elif isinstance(event, p.Refresh):
            self.full_refresh()
        elif self.connected:
            self.controller.handle(event)
            self.update()

    def tick(self):
        now = self._clock()
        self._send_all(self.session.tick(now, self.last_send))
        if not self.connected:
            return
        if self._view_deadline is not None and now >= self._view_deadline:
            self._log("no VIEW after MODES; sending the full state anyway")
            self._view_deadline = None
            self.display.invalidate()
        if self._view_deadline is None:
            self._render()
            self.controller.render_periodic(now)
            self.outbox.flush(now)

    def song_time_changed(self):
        if self.connected and self._view_deadline is None:
            self.controller.render_transport()
            self.outbox.flush(self._clock())

    def selection_changed(self):
        # The keyboard's 4D arrives in Live as arrow keys by default, so the
        # selection moves without any message to us. Redraw right away
        # rather than on the next 100 ms tick.
        if self.connected:
            self.update()

    def state_changed(self):
        self.selection_changed()

    # -- rendering ----------------------------------------------------------

    def _sync_modes(self):
        if not self.v2:
            return
        message = p.modes(self.controller.modes())
        if message != self._modes_sent:
            self._modes_sent = message
            self._send_all([message])

    def _render(self):
        self._sync_modes()
        self.controller.render()
        mode = self.controller.poll_device_selection()
        if mode is not None and self.v2:
            self.outbox.post(("set_mode",), p.set_mode(mode), PRIO_NOTIFY)

    def update(self):
        """Render and send what changed, within the rate limit."""
        if self._view_deadline is not None:
            return
        self._render()
        self.outbox.flush(self._clock())

    def full_refresh(self):
        """Forget what the device shows and send the whole current view."""
        self.display.invalidate()
        if self.connected:
            self.update()
