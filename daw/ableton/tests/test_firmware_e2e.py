# End-to-end: the script's Engine (on the Live fakes) against the real
# firmware, run as firmware/sim/build/mk-sim --stdio --fake-time.
#
# Engine output goes to the keyboard as "in 1 <hex>", the keyboard's
# "out 1 <hex>" goes back into the Engine, and "out 0" (the Keys port) is
# kept for the note checks. Both sides run on one fake clock: the rig
# advances the keyboard in 10 ms ticks and calls Engine.tick() every
# 100 ms, as Live's update_display() would. Screen checks use the sim's
# "find", not screen dumps. stall() stops the host as a busy Live would:
# no ticks, and the keyboard's messages wait until it resumes.
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import subprocess
import unittest

from fakes import Clock, FakeBrowser, FakeDevice, FakeRack, FakeSong
from MyKontrol import protocol as p
from MyKontrol.browser_view import MODE_BROWSER
from MyKontrol.engine import Engine

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SIM = os.environ.get("MYKONTROL_SIM", os.path.join(ROOT, "firmware", "sim", "build", "mk-sim"))

if not os.path.exists(SIM):
    raise unittest.SkipTest("needs the firmware simulator: run `make -C firmware sim` "
                            "(expected at %s)" % SIM)

# app_event() types, firmware/core/hal.h
EV_KEY, EV_BUTTON, EV_ENCODER, EV_KNOB, EV_KNOB_TOUCH = 0, 1, 2, 3, 4
KEYS, DAW = 0, 1
SYNC = "error unknown command sync"
SPLASH_MS = 1300
POPUP_MS = 1300      # "Connected" stays 1.2 s after HELLO


class Sim(object):
    """mk-sim --stdio --fake-time as a subprocess, one command at a time."""

    def __init__(self):
        self.proc = subprocess.Popen([SIM, "--stdio", "--fake-time"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, universal_newlines=True,
                                     bufsize=1)
        self.outs = []         # (cable, bytes) the keyboard sent, not yet taken
        self.errors = []

    def run(self, *lines):
        """Send command lines; return the reply lines other than "out"."""
        for line in lines + ("sync",):
            self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        replies = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("mk-sim exited")
            line = line.rstrip("\n")
            if line == SYNC:
                return replies
            if line.startswith("out "):
                _, cable, data = line.split(" ", 2)
                self.outs.append((int(cable), tuple(int(b, 16) for b in data.split())))
            elif line.startswith("error"):
                self.errors.append(line)
            else:
                replies.append(line)

    def take(self):
        outs, self.outs = self.outs, []
        return outs

    def close(self):
        try:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        self.proc.wait(5)
        self.proc.stdout.close()


class Rig(object):

    def __init__(self, song=None, browser=True):
        self.clock = Clock(0.0)
        self.song = song or FakeSong(n_tracks=3, n_returns=1)
        self.browser = FakeBrowser() if browser else None
        self.to_keyboard = []          # Engine output not yet given to the sim
        self.from_host = []            # everything the Engine sent
        self.from_keyboard = []        # every DAW-port message the keyboard sent
        self.notes = []                # Keys-port messages
        self.engine = Engine(self.song, self._send, clock=self.clock, host_version="12.1.5",
                             browser=(lambda: self.browser) if browser else None)
        self.sim = Sim()
        self.stalled = False
        self._held = []                # keyboard messages waiting for a stalled host
        self._next_tick = 0.1

    def _send(self, message):
        self.to_keyboard.append(tuple(message))
        self.from_host.append(tuple(message))

    def pump(self):
        """Exchange messages until both sides are quiet."""
        for _ in range(100):
            lines = ["in 1 " + " ".join("%02X" % b for b in m) for m in self.to_keyboard]
            self.to_keyboard = []
            if lines:
                self.sim.run(*lines)
            outs = self.sim.take()
            for cable, message in outs:
                if cable == DAW:
                    self.from_keyboard.append(message)
                    if self.stalled:
                        self._held.append(message)
                    else:
                        self.engine.receive(message)
                else:
                    self.notes.append(message)
            if not lines and not outs:
                return
        raise AssertionError("host and keyboard keep talking")

    def advance(self, ms, step=10):
        """Run both sides for `ms` of fake time."""
        for _ in range(int(ms // step)):
            self.clock.advance(step / 1000.0)
            self.sim.run("tick %d" % step)
            if self.clock() >= self._next_tick - 1e-9:
                self._next_tick += 0.1
                if not self.stalled:
                    self.engine.tick()
            self.pump()

    def event(self, kind, ident, value, ms=20):
        self.sim.run("event %d %d %d" % (kind, ident, value))
        self.pump()
        self.advance(ms)

    def button(self, control, ms=60):
        self.event(EV_BUTTON, control, 1, 20)
        self.event(EV_BUTTON, control, 0, ms)

    def stall(self, ms):
        self.stalled = True
        self.advance(ms)

    def resume(self):
        self.stalled = False
        held, self._held = self._held, []
        for message in held:
            self.engine.receive(message)
        self.pump()

    def step_to(self, mode, limit=8):
        """Press S until the keyboard is in `mode`."""
        for _ in range(limit):
            if self.engine.controller.view_id == mode:
                break
            self.button(p.CTL_SOLO, ms=100)
        self.advance(1500)         # the mode popup goes

    def connect(self):
        self.engine.start()
        self.pump()
        self.advance(SPLASH_MS + POPUP_MS)

    def find(self, text):
        """(x, y, font, inverted) if the screen shows text, else None."""
        reply = self.sim.run("find " + text)[-1]
        if reply == "notfound":
            return None
        words = reply.split()
        return int(words[1]), int(words[2]), words[3], "inverted" in words[4:]

    def close(self):
        self.sim.close()


def cmds(messages):
    return [m[4] for m in messages if m and m[0] == 0xF0]


def synth_song():
    song = FakeSong(n_tracks=3, n_returns=1)
    track = song.visible_tracks[0]
    synth = FakeDevice("Analog", type=1, param_names=["Cutoff", "Reso", "Drive", "Attack",
                                                        "Decay", "Sustain", "Release", "Level"])
    synth.canonical_parent = track
    track.devices[0] = synth
    track.view.selected_device = synth
    return song


def drum_song():
    song = FakeSong(n_tracks=3, n_returns=1)
    rack = FakeRack("Kit", [], drum=True)
    for note, name in ((36, "Kick"), (38, "Snare"), (41, "Low Tom"), (42, "Closed Hat"),
                       (44, "Pedal Hat"), (46, "Open Hat")):
        rack.set_pad(note, name)
    track = song.visible_tracks[0]
    rack.canonical_parent = track
    track.devices = [rack]
    track.view.selected_device = rack
    return song


class Firmware(unittest.TestCase):

    def rig(self, **kw):
        rig = Rig(**kw)
        self.addCleanup(rig.close)
        self.addCleanup(lambda: self.assertEqual(rig.sim.errors, []))
        return rig

    def assertShown(self, rig, text):
        self.assertIsNotNone(rig.find(text), "%r not on screen" % text)

    def assertNotShown(self, rig, text):
        self.assertIsNone(rig.find(text), "%r on screen" % text)

    def test_handshake(self):
        rig = self.rig(song=synth_song())
        rig.connect()
        hello = p.parse_host(rig.from_host[0])
        self.assertEqual((hello.proto, hello.name, hello.version), (2, "Ableton Live", "12.1.5"))
        ident = [p.parse(m) for m in rig.from_keyboard if m[4:5] == (p.CMD_IDENT,)]
        self.assertEqual(len(ident), 1)
        self.assertEqual((ident[0].proto, ident[0].board), (2, 1))
        self.assertTrue(ident[0].list_rows and ident[0].list_cache >= 2 * ident[0].list_rows)
        self.assertEqual(cmds(rig.from_host)[:2], [p.CMD_HELLO, p.CMD_MODES])
        self.assertIn(p.view(p.VIEW_INSTRUMENT), rig.from_keyboard)
        self.assertTrue(rig.engine.v2)
        self.assertShown(rig, "Analog")             # the device, as the page title
        self.assertShown(rig, "Cutoff")             # a parameter name

    def test_knob_turn_changes_live_and_the_value_follows(self):
        rig = self.rig(song=synth_song())
        rig.connect()
        cutoff = rig.song.visible_tracks[0].devices[0].parameters[1]
        rig.event(EV_KNOB_TOUCH, 0, 1)
        rig.event(EV_KNOB, 0, 10, ms=100)
        self.assertAlmostEqual(cutoff.value, 0.55)
        self.assertShown(rig, "0.55")
        rig.event(EV_KNOB, 0, -30, ms=100)
        self.assertAlmostEqual(cutoff.value, 0.40)
        self.assertShown(rig, "0.40")
        cutoff.value = 0.75                          # moved in Live with the mouse
        rig.advance(200)
        self.assertShown(rig, "0.75")
        rig.event(EV_KNOB_TOUCH, 0, 0)

    def test_mode_stepping_reaches_the_browser_list(self):
        rig = self.rig()
        rig.connect()
        seen = []
        for _ in range(8):
            rig.button(p.CTL_SOLO, ms=100)
            seen.append(rig.engine.controller.view_id)
            if seen[-1] == MODE_BROWSER:
                break
        # The user's order of standard modes, then ours; clips is not in it.
        self.assertEqual(seen, [p.VIEW_PLUGIN, p.VIEW_VOLUME, p.VIEW_PAN, p.VIEW_SENDS,
                                MODE_BROWSER])
        rig.advance(1500)
        self.assertShown(rig, "Browser")             # LIST title
        self.assertEqual(rig.find("Sounds")[3], True)        # row 0, under the cursor
        self.assertEqual(rig.find("Instruments")[3], False)
        rig.event(EV_ENCODER, 0, 1, ms=100)          # the 4D reaches us while a list is open
        self.assertIn((p.CONTROL_CHANGE, p.ENCODER_CC, 1), rig.from_keyboard)
        self.assertEqual(rig.find("Sounds")[3], False)
        self.assertEqual(rig.find("Instruments")[3], True)
        rig.button(p.CTL_NAV_PUSH, ms=200)           # open Instruments
        self.assertShown(rig, "Operator")
        rig.button(p.CTL_NAV_PUSH, ms=200)           # load it
        self.assertEqual(rig.browser.loaded, ["Operator"])

    def test_drum_pad_name_on_the_key_played(self):
        rig = self.rig(song=drum_song())
        rig.connect()
        notes = [key.note for key in map(p.parse_host, rig.from_host) if isinstance(key, p.Key)]
        self.assertEqual(notes[:4], [36, 38, 42, 46])        # kick, snare, hats first
        rig.event(EV_KEY, 1, 100, ms=50)
        note = rig.notes[-1]
        self.assertEqual(note[:2], (0x90, 42))       # key 1 of the M32 at octave 0
        self.assertShown(rig, "Closed Hat")
        rig.event(EV_KEY, 1, 0, ms=50)
        rig.event(EV_KEY, 0, 100, ms=50)             # note 41
        self.assertShown(rig, "Low Tom")

    def test_smart_play_follows_the_song_key(self):
        rig = self.rig()
        rig.connect()
        song = rig.song
        song.scale_mode, song.root_note, song.scale_name = True, 2, "Major"   # D major
        rig.engine.state_changed()
        rig.pump()
        rig.button(p.CTL_SCALE, ms=1500)             # Scale on (the keyboard's own button)
        played = []
        for key in range(8):                         # notes 41..48: F to C
            rig.event(EV_KEY, key, 100, ms=20)
            played.append(rig.notes[-1][1])
            rig.event(EV_KEY, key, 0, ms=20)
        d_major = {2, 4, 6, 7, 9, 11, 1}
        self.assertTrue(all(n % 12 in d_major for n in played), played)
        self.assertIn(42, played)                    # F# is in D major, not in C major
        self.assertNotIn(41, played)                 # F is not

    def lost_and_back(self, mode):
        rig = self.rig()
        rig.connect()
        rig.step_to(mode)
        sent = len(rig.from_host)
        rig.stall(3100)                              # Live busy for 3.1 s
        lost = rig.find("Connection lost")
        self.assertTrue(rig.find("Connection"), "no lost mark at all")
        rig.resume()                                 # Live reads the HI the keyboard sent
        rig.advance(1500)
        back = cmds(rig.from_host[sent:])
        return rig, lost, back

    def test_lost_host_and_recovery(self):
        rig, lost, back = self.lost_and_back(p.VIEW_VOLUME)
        self.assertIsNotNone(lost, "no 'Connection lost' after 3.1 s of silence")
        self.assertIn(p.CMD_HELLO, back)             # handshake again on HI ...
        self.assertIn(p.CMD_MODES, back)
        self.assertIn(p.CMD_PAGE, back)              # ... and the full state
        self.assertTrue(rig.engine.connected)
        self.assertNotShown(rig, "Connection lost")
        self.assertShown(rig, "Tracks 1-5")

    def test_lost_mark_fits_in_instrument_mode(self):
        _, lost, _ = self.lost_and_back(p.VIEW_INSTRUMENT)
        self.assertIsNotNone(lost)

    def test_lost_mark_over_a_list(self):
        _, lost, _ = self.lost_and_back(MODE_BROWSER)
        self.assertIsNotNone(lost)
