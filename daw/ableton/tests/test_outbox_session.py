# Outbox (change detection + rate limit) and Session (handshake) tests.
# SPDX-License-Identifier: GPL-3.0-or-later

import unittest

import fakes  # noqa: F401
from MyKontrol import protocol as p
from MyKontrol.outbox import Outbox
from MyKontrol.session import Session


class OutboxTests(unittest.TestCase):

    def setUp(self):
        self.sent = []
        self.box = Outbox(self.sent.append, max_messages=20, max_bytes=960, window=0.1)

    def test_only_changes_are_sent(self):
        self.box.put("a", (1, 2), 0)
        self.assertEqual(self.box.flush(0.0), 1)
        self.box.put("a", (1, 2), 0)
        self.assertEqual(self.box.flush(1.0), 0)
        self.box.put("a", (1, 3), 0)
        self.box.flush(2.0)
        self.assertEqual(self.sent, [(1, 2), (1, 3)])

    def test_burst_collapses_to_latest(self):
        for v in range(10):
            self.box.put("knob", (v,), 0)
        self.box.flush(0.0)
        self.assertEqual(self.sent, [(9,)])

    def test_reverting_drops_pending_change(self):
        self.box.put("a", (1,), 0)
        self.box.flush(0.0)
        self.box.put("a", (2,), 0)
        self.box.put("a", (1,), 0)
        self.assertEqual(self.box.flush(1.0), 0)

    def test_message_limit_per_window(self):
        for i in range(50):
            self.box.put(i, (i,), 0)
        self.assertEqual(self.box.flush(0.0), 20)
        self.assertEqual(self.box.flush(0.05), 0)     # same window
        self.assertEqual(self.box.flush(0.1), 20)
        self.assertEqual(self.box.flush(0.2), 10)
        self.assertEqual([m[0] for m in self.sent], list(range(50)))

    def test_sliding_window_never_exceeds_limit(self):
        for i in range(40):
            self.box.put(i, (i,), 0)
        self.box.max_messages = 10
        self.assertEqual(self.box.flush(0.00), 10)
        self.box.max_messages = 20
        self.assertEqual(self.box.flush(0.09), 10)   # 20 within 0..0.09
        self.assertEqual(self.box.flush(0.095), 0)
        self.assertEqual(self.box.flush(0.10), 10)   # the t=0 sends expired
        self.assertEqual(self.box.flush(0.15), 0)

    def test_byte_limit_per_window(self):
        for i in range(10):
            self.box.put(i, (0,) * 200, 0)
        self.assertEqual(self.box.flush(0.0), 4)      # 4 * 200 <= 960

    def test_reserve_keeps_room_for_urgent_messages(self):
        box = Outbox(self.sent.append, max_messages=20, reserve=4, bulk_priority=1)
        for i in range(40):
            box.put(i, (i,), 3)
        self.assertEqual(box.flush(0.0), 16)            # bulk stops 4 short
        box.put("hot", (99,), 0)
        self.assertEqual(box.flush(0.01), 1)            # urgent still goes out
        self.assertEqual(self.sent[-1], (99,))

    def test_priority_order(self):
        self.box.put("text", (3,), 3)
        self.box.put("light", (1,), 1)
        self.box.put("transport", (0,), 0)
        self.box.flush(0.0)
        self.assertEqual(self.sent, [(0,), (1,), (3,)])

    def test_invalidate_resends_everything(self):
        self.box.put("a", (1,), 0)
        self.box.flush(0.0)
        self.box.invalidate()
        self.box.put("a", (1,), 0)
        self.assertEqual(self.box.flush(1.0), 1)

    def test_events_are_not_deduplicated(self):
        self.box.post("n", (5,), 0)
        self.box.flush(0.0)
        self.box.post("n", (5,), 0)
        self.box.flush(1.0)
        self.assertEqual(self.sent, [(5,), (5,)])


class SessionTests(unittest.TestCase):

    def test_handshake(self):
        s = Session()
        self.assertEqual(s.start(), [p.hello()])
        self.assertFalse(s.connected)
        self.assertEqual(s.on_hi(p.Hi(1)), [p.hello()])
        msgs, full = s.on_ident(p.parse(p.ident()))
        self.assertEqual((msgs, full), ([], True))
        self.assertTrue(s.connected)
        self.assertEqual(s.stop(), [p.bye()])
        self.assertEqual(s.stop(), [])

    def test_keepalive(self):
        s = Session(ping_interval=0.5)
        self.assertEqual(s.tick(10.0, None), [])     # not connected
        s.start()
        s.on_ident(p.parse(p.ident()))
        self.assertEqual(s.tick(10.0, 9.8), [])
        self.assertEqual(s.tick(10.0, 9.5), [p.ping()])

    def test_hi_while_connected_reconnects(self):
        s = Session()
        s.start()
        s.on_ident(p.parse(p.ident()))
        self.assertEqual(s.on_hi(p.Hi(1)), [p.hello()])
        self.assertFalse(s.connected)

    def test_wrong_protocol(self):
        s = Session()
        self.assertEqual(s.on_hi(p.Hi(2)), [])
        s.start()
        msgs, full = s.on_ident(p.Ident(2, 1, (0, 0, 0), 32, 8, 0, None))
        self.assertEqual((msgs, full), ([p.bye()], False))
        self.assertFalse(s.connected)


if __name__ == "__main__":
    unittest.main()
