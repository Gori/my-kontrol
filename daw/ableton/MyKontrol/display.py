# Protocol-aware front end of the outbox: what views draw into.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Views call text() / page() / slot() / light() / grid() with plain values
# every render; the outbox turns that into "send only what changed".
# Priorities keep the time-critical messages (transport beat, button lights)
# ahead of a backlog of display text.
#
# The same calls drive both protocol versions. With a v1 keyboard, page()
# writes TEXT 0x01-0x03 and a slot is a label, a value text and a PARAM.
# With a v2 keyboard (configure()), they become PAGE / SLOT / VALUE:
#
#   - A new page (other parameters, title, bank...) gets the next gen, 1..127
#     and never 0, and the outbox forgets every SLOT / VALUE / slot METER it
#     sent, since the keyboard clears them on PAGE.
#   - SLOT waits for its PAGE and VALUE for its SLOT (Outbox `after`), so
#     nothing of a page reaches the keyboard before the page itself.
#   - VALUE carries the value text only when it changed. While a knob turns
#     and Live's text stays the same, only the position goes out.
#
# Lists (v2) go out in strict order, because the keyboard evicts the rows
# farthest from the cursor whenever its cache is full and we model that
# cache to know which rows it holds. See ListModel.

from . import protocol as p

PRIO_TRANSPORT = 0    # beat flashes
PRIO_HOT = 0          # the knob being turned / touched: name and value
PRIO_LIGHT = 1        # 1 and up is "bulk" traffic; see Outbox.reserve
PRIO_NOTIFY = 1
PRIO_GRID = 2
PRIO_PARAM = 2
PRIO_PAGE = 2         # v2 PAGE / SLOT / VALUE / METER / KEY / LIST
PRIO_TEXT = 3

# Used when a v2 IDENT leaves a field out.
DEFAULT_LIST_ROWS = 4
DEFAULT_KEY_LABELS = 16
# The keyboard expires meter levels 1 s after the last frame, so a frame
# identical to the last one may be skipped only for this long.
METER_REFRESH = 0.5


def _next_id(n):
    """1..127, wrapping from 127 to 1, never 0."""
    return n % 127 + 1


class ListModel(object):
    """What the keyboard holds of one list (one id).

    The keyboard keeps up to `cache` rows and, when a new row arrives with
    the cache full, drops the row farthest from the cursor. The spec does
    not say which of two equally far rows goes, so we never let that
    choice arise: we only ever ask for rows within `reach` of the cursor
    (2 * reach + 1 <= cache) and add them so that the rows held stay one
    unbroken run [lo, hi]. With the cache full, the end of the run away from
    the new row is then always strictly farther from the cursor than the
    new row, so the keyboard's choice is known. A cursor jump that would
    break the run opens the list again under a new id instead.
    """

    def __init__(self, key, list_id, total):
        self.key = key
        self.id = list_id
        self.total = total
        self.first = 0
        self.header = None
        self.lo = self.hi = None

    def holds(self, index):
        return self.lo is not None and self.lo <= index <= self.hi

    def add(self, index, cursor, cache):
        if self.lo is None:
            self.lo = self.hi = index
        else:
            self.lo, self.hi = min(self.lo, index), max(self.hi, index)
        while self.hi - self.lo + 1 > cache:
            if cursor - self.lo >= self.hi - cursor:
                self.lo += 1
            else:
                self.hi -= 1


class Display(object):

    def __init__(self, outbox):
        self._outbox = outbox
        self.hot_knob = None   # set by the controller on knob turn / touch
        self.ident = None
        self.v2 = False
        self._gen = 0          # gen of the newest PAGE
        self._list_id = 0
        self._seq = 0          # keys for strictly ordered (list) messages
        self._reset()

    def _reset(self):
        self._page = None          # the page the keyboard has
        self._value_text = {}      # knob -> value text last sent for this page
        self._keys = None          # key labels last sent
        self._list = None          # ListModel of the open list
        self._list_open = None     # None: unknown (send LIST 0 when closing)
        self._meters = {}          # target -> (levels, time) last queued

    # -- session ------------------------------------------------------------

    def configure(self, ident):
        """The keyboard that answered the handshake (None: not connected)."""
        self.ident = ident
        self.v2 = ident is not None and ident.proto >= p.PROTOCOL_V2
        self.invalidate()

    def invalidate(self):
        """Forget what the keyboard shows; the next render sends it all."""
        self._outbox.invalidate()
        self._reset()

    def _ident_field(self, name, default):
        value = getattr(self.ident, name, None) if self.ident is not None else None
        return value if value else default

    def list_rows(self):
        return self._ident_field("list_rows", DEFAULT_LIST_ROWS)

    def list_cache(self):
        return max(self._ident_field("list_cache", 2 * self.list_rows()), 1)

    def key_limit(self):
        return self._ident_field("key_labels", DEFAULT_KEY_LABELS)

    # -- v1 and v2 ----------------------------------------------------------

    def text(self, field, value, priority=PRIO_TEXT):
        self._outbox.put(("text", field), p.text(field, value), priority)

    def light(self, control, state):
        if control in p.DEVICE_OWNED_CONTROLS:
            return
        self._outbox.put(("light", control), p.light(control, state), PRIO_LIGHT)

    def transport(self, flags, bpm, beat, beats_per_bar, flags2=0):
        message = p.transport(flags, bpm, beat, beats_per_bar, flags2 if self.v2 else None)
        self._outbox.put(("transport",), message, PRIO_TRANSPORT)

    def position(self, value):
        """TEXT 0x04, the song position (v2)."""
        if self.v2:
            self.text(p.TEXT_POSITION, value, PRIO_PARAM)

    def grid(self, cols, rows, cursor_col, cursor_row, states):
        self._outbox.put(("grid",), p.grid(cols, rows, cursor_col, cursor_row, states),
                         PRIO_GRID)

    def notify(self, title, value):
        self._outbox.post(("notify",), p.notify(title, value), PRIO_NOTIFY)

    def page(self, mode, title, subtitle, bank=0, banks=0, identity=None, v1=("", "", "")):
        """What the knobs control. v1 sends the TEXT fields in `v1` (device,
        bank, subtitle) instead. `identity` says which parameters the page
        holds; a change starts a new page even if the titles stay."""
        if not self.v2:
            device, bank_name, sub = v1
            self.text(p.TEXT_DEVICE, device)
            self.text(p.TEXT_BANK, bank_name)
            self.text(p.TEXT_SUBTITLE, sub)
            return
        page = (identity, mode, bank, banks, title, subtitle)
        if page == self._page:
            return
        self._page = page
        self._gen = _next_id(self._gen)
        self._value_text = {}
        self._outbox.forget_where(lambda k: k[0] in ("page", "slot", "value")
                                  or k == ("meter", p.METER_SLOTS))
        self._meters.pop(p.METER_SLOTS, None)
        self._outbox.put(("page",), p.page(self._gen, mode, bank, banks, title, subtitle),
                         PRIO_PAGE)

    def knob(self, knob, label, value_text, value, flags):
        """v1: label, value text and bar for one knob in one call. The knob
        in use jumps the queue so its value text keeps up with turning."""
        hot = knob == self.hot_knob
        self.text(p.TEXT_KNOB_LABEL + knob, label, PRIO_HOT if hot else PRIO_TEXT)
        self.text(p.TEXT_KNOB_VALUE + knob, value_text, PRIO_HOT if hot else PRIO_PARAM)
        self._outbox.put(("param", knob), p.param(knob, value, flags),
                         PRIO_HOT if hot else PRIO_PARAM)

    def slot(self, knob, kind, flags, steps, default, colour, name, group, pos, value_text):
        """v2: SLOT and VALUE for one knob of the current page."""
        self._outbox.put(("slot", knob),
                         p.slot(self._gen, knob, kind, flags, steps, default, colour, name, group),
                         PRIO_PAGE, after=(("page",),))
        self.value(knob, pos, value_text)

    def value(self, knob, pos, value_text):
        key = ("value", knob)
        # The text goes when it changed, or when it would otherwise be lost
        # from a queued VALUE this one replaces.
        with_text = (self._value_text.get(knob) != value_text
                     or self._outbox.is_pending(key))
        if with_text:
            self._value_text[knob] = value_text
        self._outbox.put(key, p.value(self._gen, knob, pos, value_text if with_text else ""),
                         PRIO_HOT if knob == self.hot_knob else PRIO_PAGE,
                         after=(("page",), ("slot", knob)))

    def empty_knob(self, knob):
        if not self.v2:
            self.knob(knob, "", "", 0, 0)
            return
        if self._value_text.pop(knob, None) is not None or self._outbox.is_pending(("value", knob)):
            self._outbox.forget_where(lambda k: k == ("value", knob))
        self._outbox.put(("slot", knob),
                         p.slot(self._gen, knob, p.KIND_EMPTY, 0, 0, 0, p.NO_COLOUR, "", ""),
                         PRIO_PAGE, after=(("page",),))

    # -- v2 only ------------------------------------------------------------

    def meter(self, target, levels, now):
        """One meter frame. Unchanged frames are skipped, but while any
        level is above 0 one goes out at least every METER_REFRESH."""
        if not self.v2:
            return
        levels = list(levels)
        last = self._meters.get(target)
        if last is not None and last[0] == levels:
            if not any(levels) or now - last[1] < METER_REFRESH:
                return
            self.forget_meter(target)
        self._meters[target] = (levels, now)
        slots = target == p.METER_SLOTS
        self._outbox.put(("meter", target),
                         p.meter(target, p.METER_SIGNAL, self._gen if slots else 0, levels),
                         PRIO_PAGE, after=(("page",),) if slots else ())

    def forget_meter(self, target):
        self._meters.pop(target, None)
        self._outbox.forget_where(lambda k: k == ("meter", target))

    def scale(self, state, root, mask, name):
        if self.v2:
            self._outbox.put(("scale",), p.scale(state, root, mask, name), PRIO_PAGE)

    def keys(self, entries):
        """Key labels [(note, role, colour, label)], in priority order. Any
        change sends KEYS_CLEAR and the whole set again."""
        if not self.v2:
            return
        entries = list(entries)[:self.key_limit()]
        if entries == self._keys:
            return
        self._keys = entries
        self._outbox.forget_where(lambda k: k[0] in ("key", "keys_clear"))
        self._outbox.post(("keys_clear",), p.keys_clear(), PRIO_PAGE)
        for i, entry in enumerate(entries):
            self._outbox.put(("key", i), p.key(*entry), PRIO_PAGE, after=(("keys_clear",),))

    def close_message(self):
        if self.v2:
            self._outbox.put(("message",), p.message(0, 0, 0, ""), PRIO_TEXT)

    def _post_ordered(self, message):
        self._seq += 1
        self._outbox.post(("list", self._seq), message, PRIO_PAGE)

    def close_list(self):
        if self.v2 and self._list_open is not False:
            self._post_ordered(p.list_header(0, 0, 0, 0, ""))
            self._list_open = False
            self._list = None

    def list(self, key, title, total, cursor, row):
        """Show a list: `key` identifies its contents, row(i) returns
        (text, ROW flags) for item i."""
        if not self.v2:
            return
        rows, cache = self.list_rows(), self.list_cache()
        reach = max(0, (cache - 1) // 2)
        total = max(0, total)
        cursor = max(0, min(total - 1, cursor)) if total else 0
        want_lo, want_hi = max(0, cursor - reach), min(total - 1, cursor + reach)
        lm = self._list
        if (lm is None or lm.key != key or lm.total != total
                or (lm.lo is not None and (want_lo > lm.hi + 1 or want_hi < lm.lo - 1))):
            self._list_id = _next_id(self._list_id)
            first = lm.first if lm is not None and lm.key == key else 0
            lm = self._list = ListModel(key, self._list_id, total)
            lm.first = first
        self._list_open = True

        if cursor < lm.first:
            lm.first = cursor
        elif cursor >= lm.first + rows:
            lm.first = cursor - rows + 1
        lm.first = max(0, min(lm.first, max(0, total - rows)))
        header = (lm.id, total, cursor, lm.first, title)
        if header != lm.header:
            lm.header = header
            self._post_ordered(p.list_header(lm.id, total, cursor, lm.first, title))
        if not total:
            return

        for index in self._row_order(lm, cursor, want_lo, want_hi, rows):
            row_text, flags = row(index)
            self._post_ordered(p.row(lm.id, index, flags, row_text))
            lm.add(index, cursor, cache)

    @staticmethod
    def _row_order(lm, cursor, want_lo, want_hi, rows):
        """Missing rows of [want_lo, want_hi], visible ones first, each next
        to the rows already held."""
        vis_lo, vis_hi = max(want_lo, lm.first), min(want_hi, lm.first + rows - 1)
        lo, hi = (cursor, cursor) if lm.lo is None else (lm.lo, lm.hi)
        order = [] if lm.lo is not None else [cursor]
        for top, bottom in ((vis_hi, vis_lo), (want_hi, want_lo)):
            while hi < top or lo > bottom:
                if hi < top:
                    hi += 1
                    order.append(hi)
                if lo > bottom:
                    lo -= 1
                    order.append(lo)
        return order
