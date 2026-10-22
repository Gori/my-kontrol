# Change-detecting, rate-limited message queue towards the device.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The keyboard's USB MIDI pipe is small (about 1 KB of buffering), so the
# script must not blast its whole state every tick. Every outgoing state
# message has a key (for example ("text", 0x10)). The outbox remembers what
# the device last received per key and only queues a message when its bytes
# differ. A burst of updates to one key collapses to the latest value.
#
# flush() sends most urgent first, but never more than `max_messages`
# messages or `max_bytes` bytes within any sliding `window` seconds. What
# does not fit stays queued for a later flush.
#
# A message can name keys it must follow (`after`): it is held back while
# any of them is still queued. v2 needs this because a SLOT or VALUE that
# reaches the keyboard before its PAGE is dropped there. Pure Python, no
# Live dependency.

from collections import deque


class Outbox(object):

    # Bulk messages (priority >= bulk_priority) may only use the window up to
    # `reserve` messages / `reserve_bytes` short of the limit, so a knob
    # being turned during a full refresh still gets through at once.
    def __init__(self, send, max_messages=20, max_bytes=960, window=0.1,
                 reserve=4, reserve_bytes=160, bulk_priority=1):
        self._send = send
        self.max_messages = max_messages
        self.max_bytes = max_bytes
        self.window = window
        self.reserve = reserve
        self.reserve_bytes = reserve_bytes
        self.bulk_priority = bulk_priority
        self._sent = {}      # key -> bytes the device has
        self._pending = {}   # key -> [priority, seq, bytes, is_state, after keys]
        self._seq = 0
        self._recent = deque()   # (time, size) of sends inside the window
        self._recent_bytes = 0

    # -- queueing -----------------------------------------------------------

    def put(self, key, message, priority, after=()):
        """Queue a state message unless the device already shows it."""
        message = tuple(message)
        if self._sent.get(key) == message:
            # Back to what the device has: drop any stale pending change.
            self._pending.pop(key, None)
            return
        self._queue(key, message, priority, True, after)

    def post(self, key, message, priority, after=()):
        """Queue a one-shot event (for example NOTIFY). Not deduplicated
        against earlier sends; a newer event with the same key replaces a
        pending one."""
        self._queue(key, tuple(message), priority, False, after)

    def _queue(self, key, message, priority, is_state, after):
        entry = self._pending.get(key)
        if entry is None:
            self._seq += 1
            self._pending[key] = [priority, self._seq, message, is_state, tuple(after)]
        else:
            # Keep the original position so a busy key cannot starve others.
            entry[0] = min(entry[0], priority)
            entry[2] = message
            entry[3] = is_state
            entry[4] = tuple(after)

    def invalidate(self):
        """Forget what the device shows and drop pending messages, so the
        next render re-sends the full state."""
        self._sent.clear()
        self._pending.clear()

    def forget(self, key):
        """Forget one key (the next put() for it is always sent)."""
        self._sent.pop(key, None)

    def forget_where(self, test):
        """Forget and drop every key for which test(key) is true."""
        for table in (self._sent, self._pending):
            for key in [k for k in table if test(k)]:
                del table[key]

    def is_pending(self, key):
        return key in self._pending

    # -- sending ------------------------------------------------------------

    @property
    def pending(self):
        return len(self._pending)

    def _expire(self, now):
        while self._recent and not 0 <= now - self._recent[0][0] < self.window:
            self._recent_bytes -= self._recent.popleft()[1]

    def account(self, message, now):
        """Count a message sent around the outbox (HELLO, PING, BYE) against
        the window, so the total stays within the limit."""
        self._expire(now)
        self._recent.append((now, len(message)))
        self._recent_bytes += len(message)

    def flush(self, now):
        """Send as much as the window allows. Returns the count."""
        self._expire(now)
        sent = 0
        while self._pending:
            # One more pass when something held back by `after` may now go.
            progress, held = self._pass(now)
            sent += progress
            if not (progress and held):
                break
        return sent

    def _pass(self, now):
        order = sorted(self._pending.items(), key=lambda kv: (kv[1][0], kv[1][1]))
        sent, held = 0, False
        for key, (priority, _, message, is_state, after) in order:
            if any(k in self._pending for k in after):
                held = True
                continue
            bulk = priority >= self.bulk_priority
            max_messages = self.max_messages - (self.reserve if bulk else 0)
            max_bytes = self.max_bytes - (self.reserve_bytes if bulk else 0)
            if len(self._recent) >= max_messages:
                if bulk:
                    break
                continue
            if self._recent_bytes + len(message) > max_bytes:
                if bulk:
                    break
                continue
            del self._pending[key]
            self._send(message)
            if is_state:
                self._sent[key] = message
            self.account(message, now)
            sent += 1
        return sent, held
