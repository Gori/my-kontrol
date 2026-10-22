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
#
# It speaks protocol v2 by default (docs/protocol.md): it answers MODES
# and SET_MODE with VIEW, and M / S skip the modes the host disabled.
# --proto 1 plays older firmware, which answers any HELLO with a v1 IDENT.

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
VIEW_BY_NAME = {"instrument": p.VIEW_INSTRUMENT, "inst": p.VIEW_INSTRUMENT,
                "plugin": p.VIEW_PLUGIN, "plug-in": p.VIEW_PLUGIN, "device": p.VIEW_PLUGIN,
                "volume": p.VIEW_VOLUME, "track": p.VIEW_VOLUME,
                "pan": p.VIEW_PAN, "sends": p.VIEW_SENDS,
                "clips": p.VIEW_CLIPS, "clip": p.VIEW_CLIPS, "browser": p.VIEW_CLIPS}
# Knob modes that Mute / Solo (without Shift) step through, keyboard default.
MODE_ORDER = (p.VIEW_INSTRUMENT, p.VIEW_PLUGIN, p.VIEW_VOLUME, p.VIEW_PAN, p.VIEW_SENDS)
# View buttons the keyboard handles itself: (view, view with Shift).
# Shift+Browser opens the keyboard's menu (None: no view change).
VIEW_BUTTONS = {p.CTL_PLUGIN: (p.VIEW_INSTRUMENT, p.VIEW_PLUGIN),
                p.CTL_TRACK: (p.VIEW_VOLUME, p.VIEW_SENDS),
                p.CTL_BROWSER: (p.VIEW_CLIPS, None)}

HELP = """\
commands (controls by name or ID; knobs are 1-8):
  b <ctl>[+<ctl>...]   press and release, e.g.  b play   b shift+play   b 9
  hold <ctl> | rel <ctl>
  k <knob> <delta>     turn a knob, e.g.  k 1 +5   k 3 -10
  t <knob> [on|off]    touch a knob (no arg: touch and release)
  e <delta>            turn the 4D encoder
  up down left right push    4D buttons (prefix "shift" to hold Shift)
  view instrument|plugin|volume|pan|sends|clips|<id>   change view (sends VIEW)
  b plugin / b shift+plugin / b track / b shift+track / b browser
                       view buttons, as on the keyboard
  b mute / b solo      previous / next knob mode (handled by the keyboard);
                       b shift+mute / b shift+solo go to the host
  refresh              send REFRESH
  screen               show what the device displays
  reset                simulate a reboot (leave host mode, send HI)
  quiet                toggle printing of TRANSPORT/PING
  quit
controls: """ + " ".join(sorted(CONTROL_BY_NAME))


class Simulator(object):

    def __init__(self, port_name, caps, verbose, connect, proto=p.PROTOCOL_V2):
        self.lock = threading.RLock()
        self.screen = Screen()
        self.proto = proto
        self.session_proto = None
        self.host_mode = False
        self.view = p.VIEW_INSTRUMENT
        self.shift = False
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
            # Never attach to a real keyboard: it would answer the host too.
            if name in n and "M32" not in n:
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
                proto = min(decoded.proto, self.proto)
                self.session_proto = proto
                self.send(p.ident(board=1, fw=(0, 1, 0), keys=32, knobs=8, caps=self.caps,
                                  view=self.view, proto=proto,
                                  list_rows=self.screen.list_rows,
                                  list_cache=self.screen.list_cache,
                                  key_labels=self.screen.key_labels))
                if not self.host_mode:
                    self.say("-- %s: host mode on, sent IDENT v%d (view %s)"
                             % (describe(decoded), proto, view_name(self.view)))
                self.host_mode = True
                return
            if isinstance(decoded, p.Modes):
                self.say(describe(decoded))
                self.send(p.view(self.view))
                return
            if isinstance(decoded, p.SetMode):
                self.say(describe(decoded))
                if self.mode_enabled(decoded.mode):
                    self.set_view(decoded.mode, announce=False)
                self.send(p.view(self.view))
                return
            if isinstance(decoded, p.Bye):
                self.say("BYE")
                self.leave_host_mode("host said BYE")
                return
            if self.quiet and isinstance(decoded, (p.Ping, p.Transport, p.Meter)):
                return
            if self.quiet and isinstance(decoded, p.Text) and decoded.field == p.TEXT_POSITION:
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

    def button(self, control, pressed):
        """A button as the keyboard handles it: some stay local."""
        with self.lock:
            if control == p.CTL_SHIFT:
                self.shift = pressed
            elif control in VIEW_BUTTONS:
                target = VIEW_BUTTONS[control][1 if self.shift else 0]
                if pressed and target is not None:
                    self.set_view(target)
                return True
            elif control in (p.CTL_MUTE, p.CTL_SOLO) and not self.shift:
                if pressed:
                    self.step_mode(-1 if control == p.CTL_MUTE else 1)
                return True
            return self.control(p.button(control, pressed))

    def mode_enabled(self, mode):
        if self.screen.modes is None:
            return mode in p.VIEWS
        return any(m == mode and f & p.MODE_ENABLED for m, f, _ in self.screen.modes)

    def mode_order(self):
        """M / S order: the user's standard modes, then host modes (v2)."""
        if self.session_proto is None or self.session_proto < p.PROTOCOL_V2 \
                or self.screen.modes is None:
            return MODE_ORDER
        hosts = [m for m, _, _ in self.screen.modes if m in p.HOST_MODES]
        return tuple(m for m in MODE_ORDER + tuple(hosts) if self.mode_enabled(m))

    def step_mode(self, direction):
        order = self.mode_order() or MODE_ORDER
        if self.view in order:
            i = (order.index(self.view) + direction) % len(order)
        else:
            i = 0 if direction > 0 else len(order) - 1
        self.set_view(order[i])

    def press(self, controls):
        for c in controls:
            if not self.button(c, True):
                return
        for c in reversed(controls):
            self.button(c, False)

    def set_view(self, view_id, announce=True):
        with self.lock:
            self.view = view_id
            self.screen.mode = view_id
            if self.host_mode and announce:
                self.send(p.view(view_id))
            self.say("-- view: %s" % view_name(view_id))

    # -- screen dump --------------------------------------------------------

    def dump(self):
        s = self.screen
        lines = ["view %s, host mode %s, protocol %s" % (view_name(self.view), self.host_mode,
                                                         self.session_proto)]
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
        if s.page is not None:
            lines.append("  " + describe(s.page))
            for knob in sorted(s.slots):
                slot = s.slots[knob]
                if slot.kind == p.KIND_EMPTY:
                    continue
                pos, text = s.values.get(knob, (0, ""))
                lines.append("    knob%d %-24s %-16s %5d %s" % (knob + 1, slot.name, slot.group,
                                                             pos, text))
        for meter in s.meters.values():
            lines.append("  " + describe(meter))
        if s.scale is not None:
            lines.append("  " + describe(s.scale))
        for key in s.keys:
            lines.append("  " + describe(key))
        if s.list is not None:
            lines.append("  " + describe(s.list))
            for row in s.visible_rows():
                lines.append("    " + (describe(row) if row is not None else "(row missing)"))
        self.say("\n".join(lines))


def view_name(view_id):
    return p.VIEW_NAMES.get(view_id, "host mode %d" % view_id)


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
                sim.button(c, True)
        elif cmd in ("rel", "release") and args:
            for c in parse_controls(args[0]):
                sim.button(c, False)
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
        elif cmd == "view" and args and args[0].isdigit():
            sim.set_view(int(args[0]))
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
    ap.add_argument("--verbose", action="store_true",
                    help="also print TRANSPORT, PING, METER and the position")
    ap.add_argument("--proto", type=int, default=p.PROTOCOL_V2, choices=(1, 2),
                    help="protocol version to play (1: older firmware)")
    ap.add_argument("--script", help="run commands from this file (one per line), then stdin")
    args = ap.parse_args()

    sim = Simulator(args.port, args.caps, args.verbose, args.connect, args.proto)
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
