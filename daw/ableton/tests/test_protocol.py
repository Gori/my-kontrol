# Wire format tests for MyKontrol.protocol against docs/protocol.md.
# SPDX-License-Identifier: GPL-3.0-or-later

import unittest

import fakes  # noqa: F401  (sets up sys.path)
from MyKontrol import protocol as p

H = (0xF0, 0x7D, 0x4D, 0x4B)


class HostEncoders(unittest.TestCase):

    def test_session_messages(self):
        self.assertEqual(p.hello(), H + (0x00, 0x01, 0xF7))
        self.assertEqual(p.ping(), H + (0x03, 0xF7))
        self.assertEqual(p.bye(), H + (0x04, 0xF7))

    def test_text(self):
        self.assertEqual(p.text(0x10, "Cutoff"), H + (0x10, 0x10) + tuple(b"Cutoff") + (0xF7,))
        self.assertEqual(p.text(0x00, ""), H + (0x10, 0x00, 0xF7))

    def test_text_is_truncated_to_24_chars(self):
        msg = p.text(0x01, "x" * 40)
        self.assertEqual(len(msg), 4 + 1 + 1 + 24 + 1)

    def test_text_is_ascii(self):
        self.assertEqual(p.to_ascii("Café −3 dB"), "Cafe -3 dB")
        self.assertEqual(p.to_ascii("Groß\tBass"), "Gross Bass")
        self.assertEqual(p.to_ascii("☃"), "?")
        self.assertEqual(p.to_ascii(None), "")
        for b in p.text(0x00, "Åäö 日本 → ∞"):
            self.assertTrue(b < 0x80 or b in (0xF0, 0xF7))

    def test_param_and_light(self):
        self.assertEqual(p.param(3, 64, p.PARAM_MAPPED | p.PARAM_BIPOLAR),
                         H + (0x11, 3, 64, 3, 0xF7))
        self.assertEqual(p.param(0, 200, 0), H + (0x11, 0, 127, 0, 0xF7))
        self.assertEqual(p.light(p.CTL_PLAY, p.LIGHT_ON), H + (0x12, 9, 2, 0xF7))

    def test_transport(self):
        # 120 BPM -> 1200 = 9 << 7 | 48
        self.assertEqual(p.transport(p.TRANSPORT_PLAYING | p.TRANSPORT_LOOP, 120.0, 2, 4),
                         H + (0x13, 0x09, 9, 48, 2, 4, 0xF7))
        hi, lo = p.tempo_bytes(999.0)
        self.assertEqual((hi << 7) | lo, 9990)
        self.assertEqual(p.tempo_bytes(5000), (127, 127))

    def test_grid(self):
        states = [p.SLOT_EMPTY] * 32
        msg = p.grid(8, 4, 1, 2, states)
        self.assertEqual(msg[:9], H + (0x14, 8, 4, 1, 2))
        self.assertEqual(len(msg), 4 + 1 + 4 + 32 + 1)
        with self.assertRaises(ValueError):
            p.grid(8, 4, 0, 0, [0] * 31)

    def test_notify(self):
        self.assertEqual(p.notify("Quantize", "1/16"),
                         H + (0x15,) + tuple(b"Quantize") + (0,) + tuple(b"1/16") + (0xF7,))

    def test_all_host_messages_fit_the_firmware_sysex_buffer(self):
        # firmware/core/app.c parses sysex up to 128 bytes
        biggest = max(len(m) for m in (p.text(0x10, "x" * 99),
                                       p.grid(8, 4, 0, 0, [0] * 32),
                                       p.notify("x" * 99, "y" * 99)))
        self.assertLessEqual(biggest, 128)


class DeviceDecoder(unittest.TestCase):

    def test_hi_ident_view_refresh(self):
        self.assertEqual(p.parse(H + (0x02, 1, 0xF7)), p.Hi(1))
        self.assertEqual(p.parse(H + (0x01, 1, 1, 0, 2, 3, 32, 8, 0x0F, 0xF7)),
                         p.Ident(1, 1, (0, 2, 3), 32, 8, 0x0F, None))
        # Proposed 9th byte: current view.
        self.assertEqual(p.parse(H + (0x01, 1, 1, 0, 2, 3, 32, 8, 0x0F, 2, 0xF7)).view, 2)
        self.assertEqual(p.parse(H + (0x05, 2, 0xF7)), p.View(2))
        self.assertEqual(p.parse(H + (0x06, 0xF7)), p.Refresh())

    def test_extra_trailing_bytes_are_ignored(self):
        ident = p.parse(H + (0x01, 1, 1, 0, 2, 3, 32, 8, 0, 99, 99, 0xF7))
        self.assertEqual((ident.knobs, ident.view), (8, None))   # 99 is not a view

    def test_malformed_and_foreign(self):
        self.assertIsNone(p.parse(H + (0x01, 1, 1, 0xF7)))            # IDENT too short
        self.assertIsNone(p.parse((0xF0, 0x7E, 0x7F, 0x06, 0x02, 0xF7)))  # universal sysex
        self.assertIsNone(p.parse(H + (0x02, 1)))                       # no F7
        self.assertIsNone(p.parse(()))
        self.assertIsNone(p.parse((0x90, 9, 127)))                      # channel 1
        self.assertIsNone(p.parse((0xBF, 0x40, 1)))                     # unknown CC

    def test_buttons(self):
        self.assertEqual(p.parse((0x9F, 9, 127)), p.Button(9, True))
        self.assertEqual(p.parse((0x9F, 9, 0)), p.Button(9, False))
        self.assertEqual(p.parse((0x8F, 9, 64)), p.Button(9, False))

    def test_knobs_and_encoder(self):
        self.assertEqual(p.parse((0xBF, 0x10, 1)), p.KnobTurn(0, 1))
        self.assertEqual(p.parse((0xBF, 0x17, 63)), p.KnobTurn(7, 63))
        self.assertEqual(p.parse((0xBF, 0x12, 65)), p.KnobTurn(2, -1))
        self.assertEqual(p.parse((0xBF, 0x12, 127)), p.KnobTurn(2, -63))
        self.assertEqual(p.parse((0xBF, 0x12, 64)), p.KnobTurn(2, 0))
        self.assertEqual(p.parse((0xBF, 0x30, 66)), p.EncoderTurn(-2))
        self.assertEqual(p.parse((0x9F, 0x43, 127)), p.KnobTouch(3, True))
        self.assertEqual(p.parse((0x9F, 0x43, 0)), p.KnobTouch(3, False))

    def test_relative_roundtrip(self):
        for d in range(-63, 64):
            self.assertEqual(p.decode_relative(p.encode_relative(d)), d)
        self.assertEqual(p.decode_relative(p.encode_relative(500)), 63)


class Roundtrip(unittest.TestCase):

    def test_host_messages(self):
        cases = [
            (p.hello(), p.Hello(1)),
            (p.ping(), p.Ping()),
            (p.bye(), p.Bye()),
            (p.text(0x18, "-6.0 dB"), p.Text(0x18, "-6.0 dB")),
            (p.param(7, 100, 5), p.Param(7, 100, 5)),
            (p.light(3, 1), p.Light(3, 1)),
            (p.transport(1, 128.5, 3, 7), p.Transport(1, 128.5, 3, 7)),
            (p.notify("Arm", "On"), p.Notify("Arm", "On")),
            (p.notify("", ""), p.Notify("", "")),
        ]
        for msg, event in cases:
            self.assertEqual(p.parse_host(msg), event)
        g = p.parse_host(p.grid(2, 1, 1, 0, [3, 7]))
        self.assertEqual(g, p.Grid(2, 1, 1, 0, (3, 7)))

    def test_device_messages(self):
        self.assertEqual(p.parse(p.hi()), p.Hi(1))
        self.assertEqual(p.parse(p.ident(caps=3)), p.Ident(1, 1, (0, 1, 0), 32, 8, 3, None))
        self.assertEqual(p.parse(p.ident(view=1)).view, 1)
        self.assertEqual(p.parse(p.view(1)), p.View(1))
        self.assertEqual(p.parse(p.button(25, True)), p.Button(25, True))
        self.assertEqual(p.parse(p.knob_turn(4, -5)), p.KnobTurn(4, -5))
        self.assertEqual(p.parse(p.knob_touch(4, True)), p.KnobTouch(4, True))
        self.assertEqual(p.parse(p.encoder_turn(3)), p.EncoderTurn(3))


if __name__ == "__main__":
    unittest.main()
