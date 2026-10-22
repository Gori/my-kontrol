# Connection state machine: HI / HELLO / IDENT / PING / BYE.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   start()        -> HELLO (unsolicited, so a keyboard that is already up
#                     and still in host mode from a previous script instance
#                     answers at once instead of after its 3 s timeout)
#   HI from device -> HELLO
#   IDENT          -> connected; caller must send the full state
#   tick()         -> PING when nothing was sent for `ping_interval`
#   stop()         -> BYE
#
# A HI while connected means the keyboard dropped out of host mode (reboot,
# replug), or, with v2 firmware, that it marked us as lost after missed
# keep-alives: we answer HELLO and wait for IDENT, then send everything.
#
# `hello` is the HELLO we send. The script sends a v2 HELLO; firmware that
# only knows v1 ignores the extra bytes and answers a v1 IDENT, and the
# session then runs v1 (`proto`). Pure Python, no Live dependency.

from . import protocol as p

DISCONNECTED = "disconnected"
WAITING = "waiting"        # HELLO sent, no IDENT yet
CONNECTED = "connected"

SUPPORTED = (p.PROTOCOL_VERSION, p.PROTOCOL_V2)


class Session(object):

    # The device leaves host mode after 3 s of silence; keep well inside it.
    def __init__(self, ping_interval=0.5, log=None, hello=None):
        self.ping_interval = ping_interval
        self.state = DISCONNECTED
        self.ident = None
        self._hello = tuple(hello) if hello is not None else p.hello()
        self._log = log or (lambda *a: None)

    @property
    def connected(self):
        return self.state == CONNECTED

    @property
    def proto(self):
        """Protocol of the connected keyboard, else None."""
        return self.ident.proto if self.ident is not None else None

    def start(self):
        self.state = WAITING
        self.ident = None
        return [self._hello]

    def on_hi(self, event):
        """Returns the messages to send."""
        if event.proto < p.PROTOCOL_VERSION:
            self._log("ignoring HI for protocol", event.proto)
            return []
        if self.state == CONNECTED:
            self._log("keyboard lost us or left host mode; reconnecting")
        self.state = WAITING
        self.ident = None
        return [self._hello]

    def on_ident(self, event):
        """Returns (messages to send, True if the full state must follow)."""
        if event.proto not in SUPPORTED:
            self._log("device speaks protocol", event.proto, "- need one of", SUPPORTED)
            self.state = DISCONNECTED
            return [p.bye()], False
        self.state = CONNECTED
        self.ident = event
        self._log("connected:", describe_ident(event))
        return [], True

    def tick(self, now, last_send):
        """Keep-alive. last_send is when anything was last sent to the device."""
        if self.state == CONNECTED and (last_send is None or now - last_send >= self.ping_interval):
            return [p.ping()]
        return []

    def stop(self):
        was = self.state
        self.state = DISCONNECTED
        self.ident = None
        return [p.bye()] if was != DISCONNECTED else []


def describe_ident(ident):
    board = p.BOARD_NAMES.get(ident.board, "board %d" % ident.board)
    caps = [name for bit, name in ((p.CAP_TOUCH_STRIPS, "strips"),
                                   (p.CAP_KNOB_TOUCH, "knob touch"),
                                   (p.CAP_4D_ENCODER, "4D encoder"),
                                   (p.CAP_PEDAL_JACK, "pedal"))
            if ident.caps & bit]
    return "%s fw %d.%d.%d, protocol %d, %d keys, %d knobs%s" % (
        board, ident.fw[0], ident.fw[1], ident.fw[2], ident.proto, ident.keys, ident.knobs,
        (", " + ", ".join(caps)) if caps else "")
