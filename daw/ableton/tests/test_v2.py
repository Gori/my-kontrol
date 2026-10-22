# Protocol v2 (docs/protocol.md): encoding, handshake and what a v2
# keyboard ends up showing, checked through the simulator's Screen model.
# SPDX-License-Identifier: GPL-3.0-or-later

import unittest

from fakes import (Clock, FakeBrowser, FakeDevice, FakeParam, FakeRack, FakeSong,  # noqa: F401
                   FakeTrack, Wire)
from MyKontrol import liveutil as lu
from MyKontrol import protocol as p
from MyKontrol.browser_view import MODE_BROWSER
from MyKontrol.display import Display
from MyKontrol.engine import Engine
from MyKontrol.outbox import Outbox
from screen import Screen

H = (0xF0, 0x7D, 0x4D, 0x4B)


class Keyboard(object):
    """Drives the Engine as a v2 keyboard would: answers IDENT, and VIEW to
    every MODES and SET_MODE."""

    def __init__(self, browser=False, **ident_kw):
        self.clock = Clock()
        self.wire = Wire(self.clock)
        self.song = FakeSong(n_tracks=4, n_returns=2)
        self.browser = FakeBrowser() if browser else None
        self.engine = Engine(self.song, self.wire, clock=self.clock, host_version="12.1.5",
                             browser=(lambda: self.browser) if browser else None)
        self.ident_kw = dict(proto=p.PROTOCOL_V2, caps=0x0F, list_rows=4, list_cache=12,
                             key_labels=16)
        self.ident_kw.update(ident_kw)
        kw = self.ident_kw
        self.screen = Screen(list_rows=kw["list_rows"], list_cache=kw["list_cache"],
                             key_labels=kw["key_labels"])
        self.screen.mode = kw.get("view", p.VIEW_INSTRUMENT)
        self.answer_view = True
        self.follow_set_mode = True
        self._seen = 0

    def sync(self):
        out = []
        while True:
            new = self.wire.messages()[self._seen:]
            self._seen += len(new)
            out += new
            replies = []
            for m in new:
                event = self.screen.feed(m)
                if isinstance(event, p.Modes) and self.answer_view:
                    replies.append(self.screen.mode)
                elif isinstance(event, p.SetMode):
                    enabled = dict((mode, flags) for mode, flags, _ in self.screen.modes)
                    if self.follow_set_mode and enabled.get(event.mode, 0) & p.MODE_ENABLED:
                        self.screen.mode = event.mode
                    replies.append(self.screen.mode)
            if not replies:
                return out
            for mode in replies:
                self.engine.receive(p.view(mode))

    def connect(self):
        self.engine.start()
        self.engine.receive(p.hi())
        self.engine.receive(p.ident(**self.ident_kw))
        return self.settle()

    def settle(self, ticks=10):
        out = self.sync()
        for _ in range(ticks):
            self.clock.advance(0.1)
            self.engine.tick()
            out += self.sync()
        return out

    def send(self, message):
        self.engine.receive(message)
        return self.sync()

    def press(self, control, shift=False):
        out = []
        if shift:
            out += self.send(p.button(p.CTL_SHIFT, True))
        out += self.send(p.button(control, True))
        out += self.send(p.button(control, False))
        if shift:
            out += self.send(p.button(p.CTL_SHIFT, False))
        return out

    def view(self, mode):
        self.screen.mode = mode
        self.engine.receive(p.view(mode))
        return self.settle()

    def slot(self, knob):
        return self.screen.slots.get(knob)


def events(messages, kind):
    return [e for e in map(p.parse_host, messages) if isinstance(e, kind)]


V1_REPLACED = set(range(0x01, 0x04)) | set(range(0x10, 0x20))


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------

class Strings(unittest.TestCase):

    def test_ascii_and_line_break(self):
        self.assertEqual(p.encode_string("Hi\nthere", 24), [8] + list(b"Hi\nthere"))
        self.assertEqual(p.encode_string("", 24), [0])
        self.assertEqual(p.encode_string(None, 24), [0])
        self.assertEqual(p.encode_string("a\tb\r\nc", 24), [5] + list(b"a b\nc"))

    def test_non_ascii_is_escaped(self):
        # U+00E9: 0 << 14 | 1 << 7 | 105
        self.assertEqual(p.encode_string("é", 24), [4, 0x7F, 0, 1, 105])
        # U+1F3B9 = 127929 = 7 << 14 | 103 << 7 | 57
        self.assertEqual(p.encode_string("\U0001F3B9", 24), [4, 0x7F, 7, 103, 57])
        self.assertEqual(p.encode_string("\x7f", 24), [4, 0x7F, 0, 0, 0x7F])
        for text in ("Café −3 dB", "日本語", "Bass \U0001F3B9", "a\nb", "~ ~"):
            data = p.encode_string(text, 100)
            self.assertTrue(all(0 <= b <= 0x7F for b in data))
            self.assertEqual(p.decode_string(data, 0), (text, len(data)))

    def test_budget_cuts_on_a_character_boundary(self):
        self.assertEqual(p.encode_string("x" * 30, 24), [24] + [0x78] * 24)
        self.assertEqual(p.encode_string("abcé", 5), [3] + list(b"abc"))
        self.assertEqual(p.encode_string("abcé", 7), [7, 0x61, 0x62, 0x63, 0x7F, 0, 1, 105])
        self.assertEqual(p.encode_string("éééééé", 16), [16] + [0x7F, 0, 1, 105] * 4)
        self.assertEqual(p.encode_string("ab" + "é" * 10, 24)[0], 22)

    def test_field_budgets_in_messages(self):
        long = "é" * 40
        slot = p.parse_host(p.slot(1, 0, 1, 0, 0, 0, p.NO_COLOUR, long, long))
        self.assertEqual((slot.name, slot.group), ("é" * 6, "é" * 4))
        page = p.parse_host(p.page(1, 0, 0, 1, "T" * 30, "S" * 30))
        self.assertEqual((page.title, page.subtitle), ("T" * 24, "S" * 24))
        self.assertEqual(p.parse_host(p.row(1, 0, 0, "r" * 50)).text, "r" * 40)
        self.assertEqual(p.parse_host(p.key(36, 1, p.NO_COLOUR, "k" * 20)).label, "k" * 16)
        self.assertEqual(p.parse_host(p.value(1, 0, 0, "v" * 30)).text, "v" * 24)
        self.assertEqual(p.parse_host(p.message(1, 0, 1, "m" * 70)).text, "m" * 64)
        hello = p.parse_host(p.hello(2, 0x7F, "n" * 30, "v" * 30))
        self.assertEqual((hello.name, hello.version), ("n" * 24, "v" * 16))

    def test_every_v2_message_fits_128_bytes(self):
        long = "\U0001F3B9" * 99
        biggest = max(len(m) for m in (
            p.hello(2, 0x3FFF, long, long),
            p.page(127, 0, 0, 0, long, long),
            p.slot(127, 7, 3, 3, 127, p.POS_MAX, (127, 127, 127), long, long),
            p.value(127, 7, p.POS_MAX, long),
            p.meter(0, 0, 1, [127] * 8),
            p.scale(1, 11, 0xFFF, long),
            p.key(127, 11, (1, 1, 1), long),
            p.list_header(127, 9999, 9999, 9999, long),
            p.row(127, 9999, 15, long),
            p.message(127, 127, 3, long),
            p.modes([(m, 1, "") for m in range(6)] + [(16, 1, "Browser")])))
        self.assertLessEqual(biggest, 128)


class Messages(unittest.TestCase):

    def test_layouts(self):
        self.assertEqual(p.hello(2, 0x81, "Ableton Live", "12.1"),
                         H + (0x00, 2, 1, 1, 12) + tuple(b"Ableton Live") + (4,)
                         + tuple(b"12.1") + (0xF7,))
        self.assertEqual(p.page(5, 0, 1, 3, "Op", "Osc"),
                         H + (0x20, 5, 0, 1, 3, 2) + tuple(b"Op") + (3,) + tuple(b"Osc") + (0xF7,))
        self.assertEqual(p.slot(5, 2, p.KIND_ENUM, p.SLOT_HAS_DEFAULT, 4, 300, (10, 20, 30),
                                "W", ""),
                         H + (0x21, 5, 2, 3, 2, 4, 2, 44, 10, 20, 30, 1, 0x57, 0, 0xF7))
        self.assertEqual(p.value(5, 2, 16383, ""), H + (0x22, 5, 2, 127, 127, 0, 0xF7))
        self.assertEqual(p.meter(0, 0, 5, [1, 2]), H + (0x23, 0, 0, 5, 2, 1, 2, 0xF7))
        self.assertEqual(p.scale(1, 2, 0x6AD, ""), H + (0x24, 1, 2, 0x0D, 0x2D, 0, 0xF7))
        self.assertEqual(p.keys_clear(), H + (0x2A, 0xF7))
        self.assertEqual(p.list_header(0, 0, 0, 0, ""), H + (0x26, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7))
        self.assertEqual(p.message(0, 0, 0, ""), H + (0x28, 0, 0, 0, 0, 0xF7))
        self.assertEqual(p.set_mode(5), H + (0x0A, 5, 0xF7))
        self.assertEqual(p.transport(1, 120.0, 0, 4, 0x05), H + (0x13, 1, 9, 48, 0, 4, 5, 0xF7))

    def test_roundtrip(self):
        cases = [
            (p.modes([(5, 1, ""), (16, 0, "Browser")]),
             p.Modes(((5, 1, ""), (16, 0, "Browser")))),
            (p.page(3, 1, 1, 2, "Volume", "Tracks 9-16"),
             p.Page(3, 1, 1, 2, "Volume", "Tracks 9-16")),
            (p.slot(3, 7, 2, 1, 0, 8192, (127, 0, 1), "Pan", "Mix"),
             p.Slot(3, 7, 2, 1, 0, 8192, (127, 0, 1), "Pan", "Mix")),
            (p.value(3, 7, 1234, "-6.0 dB"), p.Value(3, 7, 1234, "-6.0 dB")),
            (p.meter(1, 0, 0, [100, 90]), p.Meter(1, 0, 0, (100, 90))),
            (p.scale(1, 9, 0xAB5, "Minor"), p.Scale(1, 9, 0xAB5, "Minor")),
            (p.key(38, 2, (1, 1, 1), "Snäre"), p.Key(38, 2, (1, 1, 1), "Snäre")),
            (p.keys_clear(), p.KeysClear()),
            (p.list_header(4, 300, 200, 198, "Samples"), p.ListHeader(4, 300, 200, 198, "Samples")),
            (p.row(4, 299, p.ROW_FOLDER, "Drums"), p.Row(4, 299, p.ROW_FOLDER, "Drums")),
            (p.message(2, 1, 3, "Hello"), p.Message(2, 1, 3, "Hello")),
            (p.light_rgb(9, 2, (0, 127, 0)), p.LightRgb(9, 2, (0, 127, 0))),
            (p.hello(2, 0x5F, "Ableton Live", "12.1.5"), p.Hello(2, 0x5F, "Ableton Live", "12.1.5")),
            (p.transport(3, 90.0, 1, 3, 4), p.Transport(3, 90.0, 1, 3, 4)),
        ]
        for message, event in cases:
            self.assertEqual(p.parse_host(message), event)

    def test_v2_ident(self):
        ident = p.parse(p.ident(proto=2, view=16, max_rx=300, list_rows=5, list_cache=15,
                                msg_chars=400, key_labels=24, label_chars=10, screen=2,
                                lights=2, light_guide=61))
        self.assertEqual(ident, p.Ident(2, 1, (0, 1, 0), 32, 8, 0, 16, 300, 5, 15, 400, 24, 10,
                                        2, 2, 61))
        v1 = p.parse(p.ident(view=1))
        self.assertEqual((v1.proto, v1.view, v1.list_rows), (1, 1, None))


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------

class Handshake(unittest.TestCase):

    def test_v2_handshake_order(self):
        k = Keyboard()
        k.engine.start()
        k.engine.receive(p.hi())
        k.answer_view = False
        k.engine.receive(p.ident(**k.ident_kw))
        out = k.sync()
        hello = p.parse_host(out[0])
        self.assertEqual((hello.proto, hello.name, hello.version),
                         (2, "Ableton Live", "12.1.5"))
        for bit in (p.FEATURE_PARAMETERS, p.FEATURE_METERS, p.FEATURE_SCALE,
                    p.FEATURE_KEY_LABELS, p.FEATURE_COLOURS, p.FEATURE_GRID):
            self.assertTrue(hello.features & bit)
        self.assertFalse(hello.features & p.FEATURE_LISTS)        # no browser given
        self.assertFalse(hello.features & p.FEATURE_MESSAGES)
        # MODES right after IDENT; nothing of the full state before VIEW.
        self.assertEqual([m[4] for m in out[-1:]], [p.CMD_MODES])
        k.clock.advance(0.1)
        k.engine.tick()
        self.assertEqual(events(k.sync(), p.Page), [])
        k.send(p.view(p.VIEW_INSTRUMENT))
        k.settle()
        self.assertEqual(k.screen.page.title, "Dev 1.1")

    def test_full_state_without_view_after_timeout(self):
        k = Keyboard()
        k.answer_view = False
        k.connect()
        self.assertIsNotNone(k.screen.page)

    def test_full_state_has_the_nothing_forms_and_no_v1_fields(self):
        k = Keyboard()
        out = k.connect()
        self.assertEqual(k.screen.keys_cleared, 1)
        self.assertIn(p.list_header(0, 0, 0, 0, ""), out)
        self.assertIn(p.message(0, 0, 0, ""), out)
        self.assertIsNotNone(k.screen.scale)
        for event in map(p.parse_host, out):
            self.assertNotIsInstance(event, p.Param)
            if isinstance(event, p.Text):
                self.assertNotIn(event.field, V1_REPLACED)
        self.assertEqual(k.screen.text(p.TEXT_TRACK), "Track 1")
        self.assertEqual(k.screen.dropped, 0)

    def test_v1_ident_falls_back_to_v1(self):
        k = Keyboard(proto=p.PROTOCOL_VERSION, view=p.VIEW_PLUGIN)
        out = k.connect()
        cmds = set(m[4] for m in out)
        self.assertFalse(cmds & {p.CMD_MODES, p.CMD_PAGE, p.CMD_SLOT, p.CMD_VALUE, p.CMD_METER,
                                 p.CMD_SCALE, p.CMD_KEYS_CLEAR, p.CMD_LIST, p.CMD_MESSAGE})
        self.assertEqual(k.screen.text(p.TEXT_DEVICE), "Dev 1.1")
        self.assertEqual(k.screen.text(p.TEXT_KNOB_LABEL), "Dev 1.1 P1")
        self.assertIsNone(k.screen.transport.flags2)
        self.assertIsNone(k.screen.text(p.TEXT_POSITION))
        k.send(p.view(MODE_BROWSER))                      # no host modes in v1
        self.assertEqual(k.engine.controller.view_id, p.VIEW_PLUGIN)

    def test_hi_while_connected_runs_the_handshake_again(self):
        k = Keyboard()
        k.connect()
        gen = k.screen.page.gen
        k.wire.clear()
        k._seen = 0
        k.send(p.hi())                                    # keyboard lost us
        self.assertFalse(k.engine.connected)
        self.assertEqual(p.parse_host(k.wire.messages()[-1]).proto, p.PROTOCOL_V2)
        k.screen.clear_v2()
        k.engine.receive(p.ident(**k.ident_kw))
        out = k.settle()
        self.assertEqual(out[0][4], p.CMD_MODES)
        self.assertNotEqual(k.screen.page.gen, gen)
        self.assertEqual(len(k.screen.slots), 8)
        self.assertEqual(len(k.screen.values), 8)
        self.assertEqual(k.screen.keys_cleared, 1)
        self.assertIn(p.message(0, 0, 0, ""), out)
        self.assertIn(p.list_header(0, 0, 0, 0, ""), out)

    def test_only_a_mode_change_resends_the_full_state(self):
        k = Keyboard()
        k.connect()
        k.wire.clear()
        k._seen = 0
        k.send(p.view(p.VIEW_INSTRUMENT))             # same mode
        k.settle()
        self.assertEqual(events(k.wire.messages(), p.Page), [])
        k.view(p.VIEW_VOLUME)                          # a real change
        self.assertEqual(len(events(k.wire.messages(), p.Page)), 1)
        self.assertEqual(len(events(k.wire.messages(), p.KeysClear)), 1)

    def test_refresh_resends_page_with_a_new_gen(self):
        k = Keyboard()
        k.connect()
        gen = k.screen.page.gen
        k.screen.clear_v2()
        k.send(p.refresh())
        k.settle()
        self.assertNotEqual(k.screen.page.gen, gen)
        self.assertEqual(len(k.screen.values), 8)
        self.assertEqual(k.screen.dropped, 0)


# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------

class Pages(unittest.TestCase):

    def test_plugin_page(self):
        k = Keyboard()
        k.song.visible_tracks[0].devices[0].parameters += [FakeParam("Extra %d" % i)
                                                            for i in range(9)]
        k.connect()
        k.view(p.VIEW_PLUGIN)
        page = k.screen.page
        self.assertEqual((page.mode, page.title, page.subtitle, page.bank, page.banks),
                         (p.VIEW_PLUGIN, "Dev 1.1", "Bank 1", 0, 3))
        slot = k.slot(0)
        self.assertEqual((slot.kind, slot.name, slot.group, slot.colour),
                         (p.KIND_CONTINUOUS, "Dev 1.1 P1", "", p.NO_COLOUR))
        self.assertEqual(k.screen.values[0], (8192, "0.50"))
        k.press(p.CTL_PRESET_DOWN)
        k.settle()
        self.assertEqual((k.screen.page.subtitle, k.screen.page.bank), ("Bank 2", 1))
        self.assertEqual(k.slot(0).name, "Extra 0")
        self.assertEqual(k.screen.dropped, 0)

    def test_slot_kinds_flags_and_defaults(self):
        k = Keyboard()
        params = k.song.visible_tracks[0].devices[0].parameters
        params[1] = FakeParam("Switch", 0, 0, 1, quantized=True)
        params[2] = FakeParam("Wave", 0, 0, 3, quantized=True, items=["Sin", "Tri", "Saw", "Sq"])
        params[3] = FakeParam("Transpose", 0, -48, 48, quantized=True)
        params[4] = FakeParam("Detune", 0.0, -1.0, 1.0, default=0.0)
        params[5] = FakeParam("Mode", 1, 0, 1, quantized=True, items=["Mono", "Poly"])
        k.connect()
        s = k.screen.slots
        self.assertEqual((s[0].kind, s[0].steps), (p.KIND_TOGGLE, 0))
        self.assertEqual((s[1].kind, s[1].steps), (p.KIND_ENUM, 4))
        self.assertEqual(k.screen.values[1], (0, "Sin"))
        self.assertEqual((s[2].kind, s[2].flags), (p.KIND_CONTINUOUS, p.SLOT_BIPOLAR))
        self.assertEqual((s[3].flags, s[3].default), (p.SLOT_BIPOLAR | p.SLOT_HAS_DEFAULT, 8192))
        self.assertEqual(s[4].kind, p.KIND_TOGGLE)
        self.assertEqual(s[5].kind, p.KIND_CONTINUOUS)

    def test_mixer_page_colours_and_defaults(self):
        k = Keyboard()
        tracks = k.song.visible_tracks
        tracks[1].color = 0
        del tracks[2].color
        k.connect()
        k.view(p.VIEW_VOLUME)
        page = k.screen.page
        self.assertEqual((page.title, page.subtitle, page.bank, page.banks),
                         ("", "Tracks 1-7", 0, 1))
        self.assertEqual(k.slot(0).colour, (0x7F, 0x1E, 0x14))
        self.assertEqual(k.slot(1).colour, p.BLACK)
        self.assertEqual(k.slot(2).colour, p.NO_COLOUR)
        self.assertEqual((k.slot(0).name, k.slot(0).flags, k.slot(0).default),
                         ("Track 1", p.SLOT_HAS_DEFAULT, 13926))
        self.assertEqual(k.slot(7).kind, p.KIND_EMPTY)
        k.view(p.VIEW_PAN)
        self.assertEqual(k.slot(0).flags, p.SLOT_BIPOLAR | p.SLOT_HAS_DEFAULT)
        k.view(p.VIEW_SENDS)
        self.assertEqual((k.screen.page.subtitle, k.slot(1).name), ("Sends A-B", "Return B"))
        self.assertEqual(k.slot(2).kind, p.KIND_EMPTY)

    def test_rack_macros_group(self):
        k = Keyboard()
        synth = FakeDevice("Synth", type=1)
        k.song.visible_tracks[0].devices = [FakeRack("Keys", [__import__("fakes").FakeChain([synth])])]
        k.connect()
        self.assertEqual((k.screen.page.title, k.screen.page.subtitle), ("Keys", "Macros"))
        self.assertEqual(k.slot(0).group, "Keys")

    def test_no_device_page(self):
        k = Keyboard()
        k.song.visible_tracks[0].devices = []
        k.song.visible_tracks[0].view.selected_device = None
        k.connect()
        k.view(p.VIEW_PLUGIN)
        self.assertEqual(k.screen.page.title, "No device")
        self.assertEqual([k.slot(i).kind for i in range(8)], [p.KIND_EMPTY] * 8)
        self.assertEqual(k.screen.values, {})
        k.view(p.VIEW_INSTRUMENT)
        self.assertEqual(k.screen.page.title, "No instrument")

    def test_gen_counts_1_to_127_and_wraps_to_1(self):
        sent = []
        display = Display(Outbox(sent.append, max_messages=10 ** 6, max_bytes=10 ** 9))
        display.configure(p.parse(p.ident(proto=2)))
        gens = []
        for i in range(300):
            display.page(0, "Dev", "Bank", identity=i)
            display.slot(0, 1, 0, 0, 0, None, "P", "", 0, "")
            display._outbox.flush(float(i))
            gens.append([p.parse_host(m) for m in sent if m[4] == p.CMD_PAGE][-1].gen)
        self.assertEqual(gens[:3], [1, 2, 3])
        self.assertEqual(gens[126:129], [127, 1, 2])
        self.assertNotIn(0, gens)
        self.assertTrue(all(a != b for a, b in zip(gens, gens[1:])))
        # Same page again: no new PAGE, SLOT or VALUE.
        before = len(sent)
        display.page(0, "Dev", "Bank", identity=299)
        display.slot(0, 1, 0, 0, 0, None, "P", "", 0, "")
        display._outbox.flush(1000.0)
        self.assertEqual(len(sent), before)

    def test_value_is_text_less_while_the_text_stays(self):
        k = Keyboard()
        param = k.song.visible_tracks[0].devices[0].parameters[1]
        param.str_for_value = lambda v: "%.1f" % v
        k.connect()
        values = []
        for _ in range(30):                    # 1/200 of the range per step
            k.clock.advance(0.01)
            values += events(k.send(p.knob_turn(0, 1)), p.Value)
        self.assertEqual(len(values), 30)
        self.assertEqual(values[-1].pos, p.position(param.value))
        texted = [v.text for v in values if v.text]
        self.assertEqual(texted, ["0.6", "0.7"])
        self.assertTrue(all(v.gen == k.screen.page.gen for v in values))
        self.assertEqual(k.screen.value_text(0), "%.1f" % param.value)

    def test_value_text_survives_a_collapsed_queue(self):
        sent = []
        box = Outbox(sent.append, max_messages=10 ** 6, max_bytes=10 ** 9)
        display = Display(box)
        display.configure(p.parse(p.ident(proto=2)))
        display.page(0, "Dev", "Bank", identity=1)
        display.slot(0, 1, 0, 0, 0, None, "P", "", 100, "A")
        box.flush(0.0)
        display.value(0, 200, "B")             # not flushed ...
        display.value(0, 300, "B")             # ... replaced: must keep "B"
        box.flush(1.0)
        self.assertEqual(p.parse_host(sent[-1]), p.Value(1, 0, 300, "B"))
        display.value(0, 400, "B")
        box.flush(2.0)
        self.assertEqual(p.parse_host(sent[-1]), p.Value(1, 0, 400, ""))

    def test_hot_value_never_overtakes_its_page(self):
        k = Keyboard(view=p.VIEW_VOLUME)
        k.connect()
        k.wire.clear()
        k._seen = 0
        k.press(p.CTL_PRESET_DOWN)     # nothing on the next page: stays
        k.song.visible_tracks.extend(FakeTrack("Extra %d" % i) for i in range(8))
        k.press(p.CTL_PRESET_DOWN)
        k.send(p.knob_turn(0, 5))
        k.settle()
        self.assertEqual(k.screen.dropped, 0)
        self.assertEqual(k.screen.page.subtitle, "Tracks 9-15")
        order = [m[4] for m in k.wire.messages() if m[4] in (p.CMD_PAGE, p.CMD_SLOT, p.CMD_VALUE)]
        self.assertEqual(order[0], p.CMD_PAGE)

    def test_rate_limit_under_load(self):
        k = Keyboard(browser=True)
        k.song.visible_tracks.extend(FakeTrack("More %d" % i) for i in range(10))
        k.connect()
        modes = (p.VIEW_INSTRUMENT, p.VIEW_PLUGIN, p.VIEW_VOLUME, p.VIEW_PAN, p.VIEW_SENDS,
                 p.VIEW_CLIPS, MODE_BROWSER)
        for i in range(600):
            k.clock.advance(0.007)
            if i % 14 == 0:
                k.song.visible_tracks[i % 4].output_meter_left = (i % 10) / 10.0
                k.engine.tick()
            if i % 100 == 0:
                k.screen.mode = modes[(i // 100) % len(modes)]
                k.engine.receive(p.view(k.screen.mode))
            if i % 3 == 0:
                k.engine.receive(p.encoder_turn(1 if i % 2 else -1))
            k.engine.receive(p.knob_turn(i % 8, 3 if i % 2 else -2))
            k.sync()
        sent = k.wire.sent
        for i, (t, _) in enumerate(sent):
            window = [m for u, m in sent[i:] if u < t + 0.1 - 1e-9]
            self.assertLessEqual(len(window), 20)
            self.assertLessEqual(sum(len(m) for m in window), 960)
        self.assertEqual(k.screen.dropped, 0)
        self.assertEqual(k.screen.list_ties, 0)

    def test_shift_touch_resets_to_default(self):
        k = Keyboard(view=p.VIEW_VOLUME)
        k.connect()
        vol = k.song.visible_tracks[0].mixer_device.volume
        vol.value = 0.3
        k.send(p.button(p.CTL_SHIFT, True))
        k.send(p.knob_touch(0, True))
        self.assertEqual(vol.value, 0.85)


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------

class Modes(unittest.TestCase):

    def test_modes_and_their_flags(self):
        k = Keyboard(browser=True)
        k.connect()
        modes = k.screen.modes
        self.assertEqual([m for m, _, _ in modes], [5, 0, 1, 3, 4, 2, 16])
        self.assertTrue(all(f & p.MODE_ENABLED for _, f, _ in modes))
        self.assertEqual(modes[-1][2], "Browser")
        self.assertEqual([n for _, _, n in modes[:-1]], [""] * 6)
        hello = k.screen.last_hello
        self.assertTrue(hello.features & p.FEATURE_LISTS)

    def test_modes_follow_availability(self):
        k = Keyboard()
        empty = FakeTrack("Audio")
        k.song.visible_tracks.append(empty)
        k.connect()
        k.wire.clear()
        k._seen = 0
        k.song.view.selected_track = empty
        k.engine.selection_changed()
        k.settle()
        sent = events(k.wire.messages(), p.Modes)
        self.assertEqual(len(sent), 1)                 # and no MODES / VIEW loop
        # The VIEW answering MODES repeats the mode: no second full state.
        self.assertEqual(len(events(k.wire.messages(), p.KeysClear)), 0)
        self.assertEqual(len(events(k.wire.messages(), p.Page)), 1)   # the new track's page
        flags = dict((m, f) for m, f, _ in sent[0].entries)
        self.assertEqual((flags[5], flags[0], flags[1], flags[4]), (0, 0, 1, 1))
        self.assertEqual(k.screen.page.title, "No instrument")
        k.song.return_tracks = []
        k.settle()
        flags = dict((m, f) for m, f, _ in k.screen.modes)
        self.assertEqual(flags[4], 0)

    def test_set_mode_when_a_device_is_clicked_in_live(self):
        k = Keyboard(view=p.VIEW_VOLUME)
        k.connect()
        track = k.song.visible_tracks[0]
        track.view.selected_device = track.devices[1]       # an effect, with the mouse
        k.settle()
        self.assertEqual(k.screen.set_modes, [p.VIEW_PLUGIN])
        self.assertEqual(k.engine.controller.view_id, p.VIEW_PLUGIN)
        self.assertEqual(k.screen.page.title, "Dev 1.2")
        track.view.selected_device = track.devices[0]       # the instrument
        k.settle()
        self.assertEqual(k.screen.set_modes[-1], p.VIEW_INSTRUMENT)
        self.assertEqual(k.engine.controller.view_id, p.VIEW_INSTRUMENT)

    def test_no_set_mode_for_own_navigation_or_track_changes(self):
        k = Keyboard(view=p.VIEW_PLUGIN)
        k.connect()
        k.press(p.CTL_PRESET_DOWN, shift=True)             # next device, from the keyboard
        k.settle()
        self.assertEqual(k.screen.page.title, "Dev 1.2")
        k.song.view.selected_track = k.song.visible_tracks[2]
        k.engine.selection_changed()
        k.settle()
        self.assertEqual(k.screen.set_modes, [])

    def test_set_mode_refused_by_the_keyboard(self):
        k = Keyboard(view=p.VIEW_VOLUME)
        k.follow_set_mode = False
        k.connect()
        track = k.song.visible_tracks[0]
        track.view.selected_device = track.devices[1]
        k.settle(2)
        self.assertEqual(k.screen.set_modes, [p.VIEW_PLUGIN])
        self.assertEqual(k.engine.controller.view_id, p.VIEW_VOLUME)
        self.assertEqual(k.screen.page.mode, p.VIEW_VOLUME)
        self.assertEqual(len(events(k.wire.messages(), p.KeysClear)), 1)   # handshake only


# ---------------------------------------------------------------------------
# Meters, transport, scale, keys
# ---------------------------------------------------------------------------

class Meters(unittest.TestCase):

    def test_level_mapping(self):
        self.assertEqual(lu.meter_level(0.0), 0)
        self.assertEqual(lu.meter_level(0.85), 127)
        self.assertEqual(lu.meter_level(1.0), 127)
        self.assertEqual(lu.meter_level(0.1), 0)               # below -60 dB
        levels = [lu.meter_level(v / 100.0) for v in range(13, 86)]
        self.assertEqual(levels, sorted(levels))
        self.assertTrue(1 <= levels[0] < 10)

    def test_slot_meters_in_mixer_modes(self):
        k = Keyboard(view=p.VIEW_VOLUME)
        k.connect()
        k.song.visible_tracks[0].output_meter_left = 0.85
        k.song.visible_tracks[1].output_meter_right = 0.5
        k.settle(1)
        meter = k.screen.meters[p.METER_SLOTS]
        self.assertEqual((meter.kind, meter.gen), (p.METER_SIGNAL, k.screen.page.gen))
        self.assertEqual(meter.levels[0], 127)
        self.assertEqual(meter.levels[1], lu.meter_level(0.5))
        self.assertEqual(meter.levels[4:], (0, 0, 0, 0))
        k.view(p.VIEW_SENDS)
        k.song.return_tracks[1].output_meter_left = 0.85
        k.settle(1)
        self.assertEqual(k.screen.meters[p.METER_SLOTS].levels[:2], (0, 127))

    def test_unchanged_levels_are_refreshed_every_500_ms(self):
        k = Keyboard()
        k.connect()
        track = k.song.visible_tracks[0]
        track.output_meter_left = track.output_meter_right = 0.6
        k.wire.clear()
        k._seen = 0
        k.settle(30)                                    # 3 s of the same level
        times = [t for t, m in k.wire.sent if m[4] == p.CMD_METER]
        self.assertTrue(6 <= len(times) <= 8, times)
        self.assertTrue(all(0.5 - 1e-9 <= b - a <= 0.6 + 1e-9 for a, b in zip(times, times[1:])))
        track.output_meter_left = track.output_meter_right = 0.0
        k.wire.clear()
        k._seen = 0
        k.settle(30)                                    # silence: one frame, then nothing
        self.assertEqual(len([m for _, m in k.wire.sent if m[4] == p.CMD_METER]), 1)
        self.assertEqual(k.screen.meters[p.METER_CHANNEL].levels, (0, 0))

    def test_channel_meter_in_device_modes_at_most_20_per_second(self):
        k = Keyboard()
        k.connect()
        k.wire.clear()
        k._seen = 0
        track = k.song.visible_tracks[0]
        for i in range(20):
            track.output_meter_left = 0.4 + i * 0.02
            track.output_meter_right = 0.3
            k.clock.advance(0.05)
            k.engine.tick()
            k.engine.song_time_changed()
            k.engine.selection_changed()
            k.sync()
        meters = [(t, m) for t, m in k.wire.sent if m[4] == p.CMD_METER]
        self.assertTrue(all(p.parse_host(m).target == p.METER_CHANNEL for _, m in meters))
        self.assertLessEqual(len(meters), 20)                   # 1 s of calls
        self.assertGreaterEqual(len(meters), 15)
        times = [t for t, _ in meters]
        self.assertTrue(all(b - a >= 0.05 - 1e-9 for a, b in zip(times, times[1:])))
        self.assertEqual(k.screen.meters[p.METER_CHANNEL].levels[1], lu.meter_level(0.3))


class Transport(unittest.TestCase):

    def test_second_flags_byte_and_position(self):
        k = Keyboard()
        k.connect()
        self.assertEqual(k.screen.transport.flags2, 0)
        k.song.punch_out = True
        k.song.is_counting_in = True
        k.song.view.follow_song = True
        k.song.bars, k.song.beats, k.song.sixteenth = 12, 3, 4
        k.engine.song_time_changed()
        k.sync()
        self.assertEqual(k.screen.transport.flags2, p.TRANSPORT2_PUNCH | p.TRANSPORT2_COUNT_IN
                         | p.TRANSPORT2_FOLLOW)
        self.assertEqual(k.screen.text(p.TEXT_POSITION), "12.3.4")


class Scale(unittest.TestCase):

    def test_song_key(self):
        k = Keyboard()
        k.connect()
        self.assertEqual(k.screen.scale, p.Scale(p.SCALE_NONE, 0, 1, ""))
        s = k.song
        s.scale_mode, s.root_note, s.scale_name = True, 2, "Dorian"
        s.scale_intervals = [0, 2, 3, 5, 7, 9, 10]
        k.engine.state_changed()
        k.sync()
        mask = sum(1 << i for i in (0, 2, 3, 5, 7, 9, 10))
        self.assertEqual(k.screen.scale, p.Scale(p.SCALE_SONG, 2, mask, "Dorian"))
        s.scale_intervals = [0, 1, 4, 5, 7, 8, 10]           # changed in Live
        k.settle(1)
        self.assertEqual(k.screen.scale.mask, sum(1 << i for i in (0, 1, 4, 5, 7, 8, 10)))

    def test_older_live_has_no_key(self):
        song = FakeSong()
        del song.scale_mode
        self.assertEqual(lu.scale_state(song), (p.SCALE_NONE, 0, 1, ""))


class KeyLabels(unittest.TestCase):

    PADS = [(39, "Clap"), (41, "Tom Low"), (36, "Kick 1"), (38, "Snare"), (42, "Closed Hat"),
            (46, "Open Hat"), (51, "Ride"), (70, "Shaker"), (60, "FX Zap"), (44, "Pedal HH"),
            (37, "Rimshot")]

    def drums(self):
        rack = FakeRack("Kit", [], drum=True)
        for note, name in self.PADS:
            rack.set_pad(note, name, colour=0x00FF00 if note == 36 else None)
        return rack

    def test_roles(self):
        cases = {"Kick": 1, "BD 909": 1, "Snare Hard": 2, "SD": 2, "Rim": 3, "Side Stick": 3,
                 "Clap": 4, "Closed Hat": 5, "Hihat": 5, "CHH": 5, "Pedal Hat": 6,
                 "Open Hat": 7, "OHH": 7, "Hat Open": 7, "Floor Tom": 8, "LowTom": 8,
                 "Crash": 9, "Ride Bell": 9, "Cymbal": 9, "Perc 2": 10, "Conga": 10,
                 "Tambourine": 10, "Bongo": 10, "Shaker": 10, "FX": 11, "Custom": 11,
                 "Grime": 11, "": 11}
        for name, role in cases.items():
            self.assertEqual(lu.key_role(name), role, name)

    def test_drum_rack_keys_in_priority_order(self):
        k = Keyboard()
        track = k.song.visible_tracks[0]
        track.devices = [self.drums()]
        k.connect()
        notes = [key.note for key in k.screen.keys]
        self.assertEqual(notes, [36, 38, 42, 46, 44, 39, 37, 41, 51, 70, 60])
        roles = [key.role for key in k.screen.keys]
        self.assertEqual(roles, [1, 2, 5, 7, 6, 4, 3, 8, 9, 10, 11])
        self.assertEqual(k.screen.key_for(36).colour, (0, 0x7F, 0))
        self.assertEqual(k.screen.key_for(38).colour, p.NO_COLOUR)
        self.assertEqual(k.screen.key_for(46).label, "Open Hat")

    def test_key_limit_from_ident(self):
        k = Keyboard(key_labels=4)
        k.song.visible_tracks[0].devices = [self.drums()]
        out = k.connect()
        self.assertEqual(len(events(out, p.Key)), 4)
        self.assertEqual([key.note for key in k.screen.keys], [36, 38, 42, 46])

    def test_cleared_on_a_track_without_drums(self):
        k = Keyboard()
        k.song.visible_tracks[0].devices = [self.drums()]
        k.connect()
        k.wire.clear()
        k._seen = 0
        k.song.view.selected_track = k.song.visible_tracks[1]
        k.engine.selection_changed()
        k.settle()
        self.assertEqual(k.screen.keys, [])
        self.assertEqual(len(events(k.wire.messages(), p.KeysClear)), 1)
        self.assertEqual(events(k.wire.messages(), p.Key), [])

    def test_pad_renamed_in_live(self):
        k = Keyboard()
        rack = self.drums()
        k.song.visible_tracks[0].devices = [rack]
        k.connect()
        rack.drum_pads[60].name = "Kick 2"
        k.settle(12)                                    # rescanned within about 1 s
        self.assertEqual([key.note for key in k.screen.keys][:2], [36, 60])


# ---------------------------------------------------------------------------
# Browser list
# ---------------------------------------------------------------------------

class Browser(unittest.TestCase):

    def open(self, **kw):
        k = Keyboard(browser=True, **kw)
        k.connect()
        k.view(MODE_BROWSER)
        return k

    def test_navigate_and_load(self):
        k = self.open()
        lst = k.screen.list
        self.assertEqual((lst.title, lst.total, lst.cursor), ("Browser", 4, 0))
        self.assertEqual([r.text for r in k.screen.visible_rows()],
                         ["Sounds", "Instruments", "Audio Effects", "My Folder"])
        self.assertTrue(all(r.flags & p.ROW_FOLDER for r in k.screen.visible_rows()))
        self.assertEqual((k.screen.page.mode, k.screen.page.title), (MODE_BROWSER, "Browser"))
        self.assertEqual([k.slot(i).kind for i in range(8)], [p.KIND_EMPTY] * 8)
        k.send(p.encoder_turn(1))
        k.press(p.CTL_NAV_PUSH)                       # open "Instruments"
        k.settle(1)
        lst = k.screen.list
        self.assertEqual((lst.title, lst.total, lst.cursor), ("Instruments", 2, 0))
        self.assertEqual([r.text for r in k.screen.visible_rows()], ["Operator", "Drum Rack"])
        k.press(p.CTL_NAV_RIGHT)                      # Operator's presets
        k.settle(1)
        self.assertEqual(k.screen.list.title, "Operator")
        k.send(p.encoder_turn(2))
        k.press(p.CTL_NAV_PUSH)
        k.settle(1)
        self.assertEqual(k.browser.loaded, ["Preset 3"])
        self.assertEqual(k.screen.notifications[-1], ("Loaded", "Preset 3"))
        k.press(p.CTL_NAV_LEFT)
        k.press(p.CTL_NAV_PUSH)                       # load Operator itself
        self.assertEqual(k.browser.loaded, ["Preset 3", "Operator"])
        k.press(p.CTL_NAV_LEFT)
        k.press(p.CTL_NAV_LEFT)                       # at the top: stays
        k.settle(1)
        self.assertEqual((k.screen.list.title, k.screen.list.cursor), ("Browser", 1))
        self.assertEqual(k.screen.dropped, 0)
        k.view(p.VIEW_VOLUME)
        self.assertIsNone(k.screen.list)

    def test_no_set_mode_while_loading(self):
        k = self.open()
        track = k.song.visible_tracks[0]
        track.view.selected_device = track.devices[1]   # what a load does
        k.settle(2)
        self.assertEqual(k.screen.set_modes, [])

    def check_scrolling(self, list_rows, list_cache):
        k = self.open(list_rows=list_rows, list_cache=list_cache)
        k.send(p.encoder_turn(2))
        k.press(p.CTL_NAV_PUSH)                          # Audio Effects, 40 items
        k.settle(1)
        self.assertEqual(k.screen.list.total, 40)
        moves = [1] * 45 + [-1] * 10 + [-20, 15, 3] + [-1] * 30 + [39]
        for delta in moves:
            k.send(p.encoder_turn(delta))
            k.settle(1)
            lst = k.screen.list
            self.assertTrue(lst.first <= lst.cursor < lst.first + list_rows)
            rows = k.screen.visible_rows()
            self.assertTrue(all(r is not None for r in rows), (delta, lst, sorted(k.screen.rows)))
            self.assertEqual([r.text for r in rows],
                             ["Effect %02d" % i for i in range(lst.first, lst.first + len(rows))])
            self.assertLessEqual(len(k.screen.rows), list_cache)
        self.assertEqual(k.screen.list_ties, 0)
        return k

    def test_overscan_and_cache_model(self):
        k = self.check_scrolling(4, 12)
        # One step costs one LIST and at most one ROW.
        k.wire.clear()
        k._seen = 0
        k.send(p.encoder_turn(-1))
        k.send(p.encoder_turn(-1))
        cmds = [m[4] for m in k.wire.messages()]
        self.assertEqual(cmds.count(p.CMD_LIST), 2)
        self.assertLessEqual(cmds.count(p.CMD_ROW), 2)

    def test_other_cache_sizes(self):
        for rows, cache in ((4, 8), (4, 9), (3, 7), (5, 30), (2, 4)):
            self.check_scrolling(rows, cache)

    def test_preset_buttons_page_the_list(self):
        k = self.open()
        k.send(p.encoder_turn(2))
        k.press(p.CTL_NAV_PUSH)
        k.settle(1)
        k.press(p.CTL_PRESET_DOWN)
        k.settle(1)
        self.assertEqual(k.screen.list.cursor, 4)
        k.press(p.CTL_PRESET_UP)
        k.settle(1)
        self.assertEqual(k.screen.list.cursor, 0)


if __name__ == "__main__":
    unittest.main()
