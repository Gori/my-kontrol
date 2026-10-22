#!/usr/bin/env python3
# My Kontrol device simulator: plays the keyboard's side of the host
# protocol on a virtual MIDI port, so the Live script can be developed and
# tested without hardware.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage:
#   pip install python-rtmidi
#   python3 sim/mk_sim.py
# Then in Live: Preferences > Link, Tempo & MIDI > Control Surface
# "MyKontrol", Input and Output "My Kontrol DAW". Type "help" here.
#
# Virtual ports need macOS or Linux (ALSA); on Windows use loopMIDI and
# --connect to attach to existing ports instead.

import argparse
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

from MyKontrol import protocol as p  # noqa: E402
from screen import Screen, describe, TEXT_FIELD_NAMES, LIGHT_NAMES  # noqa: E402

try:
    import rtmidi
except ImportError:
    sys.exit("mk_sim needs python-rtmidi:  pip install python-rtmidi")

HI_INTERVAL = 2.0       # device sends HI this often while no host is connected
HOST_TIMEOUT = 3.0      # device leaves host mode after this much host silence
RATE_WINDOW = 0.1       # rate check: the script promises <= 20 msgs per 100 ms
RATE_LIMIT = 20
BYTE_LIMIT = 1024

CONTROL_BY_NAME = {name: cid for cid, name in p.CONTROL_NAMES.items()}
VIEW_BY_NAME = {"device": p.VIEW_DEVICE, "plugin": p.VIEW_DEVICE,
                "mixer": p.VIEW_MIXER, "track": p.VIEW_MIXER,
                "clips": p.VIEW_CLIPS, "clip": p.VIEW_CLIPS, "browser": p.VIEW_CLIPS}

HELP = """\
commands (controls by name or ID; knobs are 1-8):
  b <ctl>[+<ctl>...]   press and release, e.g.  b play   b shift+play   b 9
  hold <ctl> | rel <ctl>
  k <knob> <delta>     turn a knob, e.g.  k 1 +5   k 3 -10
  t <knob> [on|off]    touch a knob (no arg: touch and release)
  e <delta>            turn the 4D encoder
  up down left right push    4D buttons (prefix "shift" to hold Shift)
  view device|mixer|clips    view buttons (sends VIEW)
  refresh              send REFRESH
  screen               show what the device displays
  reset                simulate a reboot (leave host mode, send HI)
  quiet                toggle printing of TRANSPORT/PING
  quit
controls: """ + " ".join(sorted(CONTROL_BY_NAME))


class Simulator(object):

    def __init__(self, port_name, caps, verbose, connect):
        self.lock = threading.RLock()
        self.screen = Screen()
        self.host_mode = False
        self.view = p.VIEW_DEVICE
        self.last_rx = 0.0
        self.last_hi = 0.0
        self.quiet = not verbose
        self.caps = caps
        self.recent = []          # (time, size) of host messages, for the rate check
        self.running = True

        self.midi_out = rtmidi.MidiOut(name="My Kontrol Sim")
        self.midi_in = rtmidi.MidiIn(name="My Kontrol Sim")
        if connect:
            self._open_existing(self.midi_out, port_name, "output")
            self._open_existing(self.midi_in, port_name, "input")
        else:
            self.midi_out.open_virtual_port(port_name)
            self.midi_in.open_virtual_port(port_name)
        self.midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
        self.midi_in.set_callback(self._on_midi)

    @staticmethod
    def _open_existing(port, name, kind):
        names = port.get_ports()
        for i, n in enumerate(names):
            if name in n:
                port.open_port(i)
                return
        sys.exit("no MIDI %s matching %r; have: %s" % (kind, name, names))

    # -- output -------------------------------------------------------------

    def say(self, text):
        with self.lock:
            print(text, flush=True)

    def send(self, message):
        self.midi_out.send_message(list(message))

    # -- device behaviour ---------------------------------------------------

    def leave_host_mode(self, why):
        with self.lock:
            if self.host_mode:
                self.say("-- host mode off (%s)" % why)
            self.host_mode = False
            self.last_hi = 0.0      # send HI right away

    def _on_midi(self, event, data=None):
        message, _ = event
        now = time.monotonic()
        with self.lock:
            self.last_rx = now
            self._check_rate(now, len(message))
            decoded = self.screen.feed(message)
            if decoded is None:
                self.say("?? %s" % " ".join("%02X" % b for b in message))
                return
            if isinstance(decoded, p.Hello):
                if decoded.proto != p.PROTOCOL_VERSION:
                    self.say("-- HELLO for protocol %d ignored" % decoded.proto)
                    return
                # The 9th IDENT byte (current view) is a proposed extension;
                # see README.md "Spec gaps".
                self.send(p.ident(board=1, fw=(0, 1, 0), keys=32, knobs=8, caps=self.caps,
                                  view=self.view))
                if not self.host_mode:
                    self.say("-- HELLO: host mode on, sent IDENT (view %s)"
                             % p.VIEW_NAMES[self.view])
                self.host_mode = True
                return
            if isinstance(decoded, p.Bye):
                self.say("BYE")
                self.leave_host_mode("host said BYE")
                return
            if self.quiet and isinstance(decoded, (p.Ping, p.Transport)):
                return
            self.say(describe(decoded))

    def _check_rate(self, now, size):
        self.recent = [(t, s) for t, s in self.recent if now - t < RATE_WINDOW]
        self.recent.append((now, size))
        count, total = len(self.recent), sum(s for _, s in self.recent)
        if count > RATE_LIMIT or total > BYTE_LIMIT:
            self.say("!! rate: %d messages / %d bytes in %d ms"
                     % (count, total, RATE_WINDOW * 1000))

    def heartbeat(self):
        """HI while no host; drop out of host mode on host silence."""
        while self.running:
            now = time.monotonic()
            with self.lock:
                if self.host_mode and now - self.last_rx > HOST_TIMEOUT:
                    self.leave_host_mode("no message from host for %.0f s" % HOST_TIMEOUT)
                if not self.host_mode and now - self.last_hi >= HI_INTERVAL:
                    self.send(p.hi())
                    self.last_hi = now
            time.sleep(0.05)

    # -- controls -----------------------------------------------------------

    def control(self, message):
        with self.lock:
            if not self.host_mode:
                self.say("-- not in host mode; the keyboard would handle this itself")
                return False
            self.send(message)
            return True

    def press(self, controls):
        for c in controls:
            if not self.control(p.button(c, True)):
                return
        for c in reversed(controls):
            self.control(p.button(c, False))

    def set_view(self, view_id):
        with self.lock:
            self.view = view_id
            if self.host_mode:
                self.send(p.view(view_id))
            self.say("-- view: %s" % p.VIEW_NAMES[view_id])

    # -- screen dump --------------------------------------------------------

    def dump(self):
        s = self.screen
        lines = ["view %s, host mode %s" % (p.VIEW_NAMES[self.view], self.host_mode)]
        for field in sorted(s.texts):
            if s.texts[field]:
                lines.append("  %-12s %s" % (TEXT_FIELD_NAMES.get(field, hex(field)),
                                            s.texts[field]))
        for knob in sorted(s.params):
            value, flags = s.params[knob]
            lines.append("  knob%d bar  %3d flags %d" % (knob + 1, value, flags))
        lit = ["%s=%s" % (p.CONTROL_NAMES.get(c, c), LIGHT_NAMES.get(v, v))
               for c, v in sorted(s.lights.items()) if v]
        lines.append("  lights: " + " ".join(lit))
        if s.transport:
            lines.append("  " + describe(s.transport))
        for row in s.grid_rows():
            lines.append("  |" + row + "|")
        self.say("\n".join(lines))


def parse_controls(spec):
    out = []
    for name in spec.lower().split("+"):
        if name.isdigit():
            out.append(int(name))
        elif name in CONTROL_BY_NAME:
            out.append(CONTROL_BY_NAME[name])
        else:
            raise ValueError("unknown control %r" % name)
    return out


def run_command(sim, line):
    words = line.split()
    if not words:
        return True
    cmd, args = words[0].lower(), words[1:]
    try:
        if cmd in ("quit", "exit", "q"):
            return False
        if cmd in ("help", "?"):
            sim.say(HELP)
        elif cmd == "b" and args:
            sim.press(parse_controls(args[0]))
        elif cmd == "hold" and args:
            for c in parse_controls(args[0]):
                sim.control(p.button(c, True))
        elif cmd in ("rel", "release") and args:
            for c in parse_controls(args[0]):
                sim.control(p.button(c, False))
        elif cmd == "k" and len(args) == 2:
            sim.control(p.knob_turn(int(args[0]) - 1, int(args[1])))
        elif cmd == "t" and args:
            knob = int(args[0]) - 1
            if len(args) == 1 or args[1] == "on":
                sim.control(p.knob_touch(knob, True))
            if len(args) == 1 or args[1] == "off":
                sim.control(p.knob_touch(knob, False))
        elif cmd == "e" and args:
            sim.control(p.encoder_turn(int(args[0])))
        elif cmd in ("up", "down", "left", "right", "push"):
            sim.press([CONTROL_BY_NAME[cmd]])
        elif cmd == "shift" and args and args[0] in ("up", "down", "left", "right", "push"):
            sim.press([p.CTL_SHIFT, CONTROL_BY_NAME[args[0]]])
        elif cmd == "view" and args and args[0] in VIEW_BY_NAME:
            sim.set_view(VIEW_BY_NAME[args[0]])
        elif cmd == "refresh":
            sim.control(p.refresh())
        elif cmd == "screen":
            sim.dump()
        elif cmd == "reset":
            sim.screen.reset()
            sim.leave_host_mode("reset")
        elif cmd == "quiet":
            sim.quiet = not sim.quiet
            sim.say("-- transport/ping printing %s" % ("off" if sim.quiet else "on"))
        else:
            sim.say("?? %s  (type help)" % line.strip())
    except ValueError as e:
        sim.say("?? %s" % e)
    return True


def main():
    ap = argparse.ArgumentParser(description="My Kontrol keyboard simulator (DAW port).")
    ap.add_argument("--port", default="My Kontrol DAW", help="virtual port name")
    ap.add_argument("--connect", action="store_true",
                    help="attach to existing ports containing --port instead of "
                         "creating virtual ones (Windows/loopMIDI)")
    ap.add_argument("--caps", type=int, default=p.CAP_TOUCH_STRIPS | p.CAP_KNOB_TOUCH
                    | p.CAP_4D_ENCODER | p.CAP_PEDAL_JACK, help="IDENT caps bits")
    ap.add_argument("--verbose", action="store_true", help="also print TRANSPORT and PING")
    ap.add_argument("--script", help="run commands from this file (one per line), then stdin")
    args = ap.parse_args()

    sim = Simulator(args.port, args.caps, args.verbose, args.connect)
    threading.Thread(target=sim.heartbeat, daemon=True).start()
    sim.say("My Kontrol simulator on %r. Select it in Live; type help." % args.port)

    try:
        if args.script:
            with open(args.script) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("sleep "):
                        time.sleep(float(line.split()[1]))
                    elif line and not line.startswith("#"):
                        sim.say("> " + line)
                        if not run_command(sim, line):
                            return
        while True:
            try:
                line = input()
            except EOFError:
                break
            if not run_command(sim, line):
                break
    except KeyboardInterrupt:
        pass
    finally:
        sim.running = False
        sim.midi_in.close_port()
        sim.midi_out.close_port()


if __name__ == "__main__":
    main()
