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
#   start() / stop()      script load / unload
#
# Keeping this free of Live imports means the unit tests exercise the real
# code path from bytes in to bytes out.

import time

from . import protocol as p
from .controller import Controller
from .display import Display
from .outbox import Outbox
from .session import Session


class Engine(object):

    def __init__(self, song, send, clock=time.monotonic, banks=None, highlight=None,
                 log=None, max_messages=20, max_bytes=960, window=0.1):
        self._send_raw = send
        self._clock = clock
        self._log = log or (lambda *a: None)
        self.last_send = None
        self.outbox = Outbox(self._send, max_messages=max_messages,
                             max_bytes=max_bytes, window=window)
        self.display = Display(self.outbox)
        self.session = Session(log=self._log)
        self.controller = Controller(song, self.display, banks=banks, highlight=highlight)

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

    # -- lifecycle ----------------------------------------------------------

    def start(self):
        self.outbox.invalidate()
        self._send_all(self.session.start())

    def stop(self):
        self.controller.shutdown()
        self.outbox.invalidate()
        self._send_all(self.session.stop())

    # -- Live callbacks -----------------------------------------------------

    def receive(self, midi_bytes):
        event = p.parse(midi_bytes)
        if event is None:
            return
        if isinstance(event, p.Hi):
            self.outbox.invalidate()
            self._send_all(self.session.on_hi(event))
        elif isinstance(event, p.Ident):
            messages, full = self.session.on_ident(event)
            self._send_all(messages)
            if full:
                self.controller.shift = False   # a fresh device has no key held
                if event.view is not None:
                    self.controller.set_view(event.view)
                self.full_refresh()
        elif isinstance(event, p.View):
            if self.controller.set_view(event.view):
                self.full_refresh()
        elif isinstance(event, p.Refresh):
            self.full_refresh()
        elif self.connected:
            self.controller.handle(event)
            self.update()

    def tick(self):
        now = self._clock()
        self._send_all(self.session.tick(now, self.last_send))
        if self.connected:
            self.controller.render()
            self.outbox.flush(now)

    def song_time_changed(self):
        if self.connected:
            self.controller.render_transport()
            self.outbox.flush(self._clock())

    # -- rendering ----------------------------------------------------------

    def update(self):
        """Render and send what changed, within the rate limit."""
        self.controller.render()
        self.outbox.flush(self._clock())

    def full_refresh(self):
        """Forget what the device shows and send the whole current view."""
        self.outbox.invalidate()
        if self.connected:
            self.update()
