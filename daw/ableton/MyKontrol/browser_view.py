# Browser mode (v2 host-defined mode 16): Live's browser as a LIST.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   4D turn, up / down    move the cursor
#   4D push               load the item onto the selected track (as a double
#                         click in Live's browser would); open a folder
#   4D right              open the item (a folder, or a device's presets)
#   4D left               back to the parent folder
#   Preset up / down      page the list
#   Knobs                 nothing (the PAGE has no slots)
#
# While a list is open the keyboard sends the 4D as protocol messages even
# with "4D as Keys" on. The list starts at Live's browser categories
# (Sounds, Drums, Instruments...) and the user's folders. A folder's items
# are read once when it is opened.
#
# `browser` is a callable returning Live's Browser (Application.browser),
# so this module does not import Live. Only offered with a v2 keyboard.

from . import protocol as p
from . import liveutil as lu
from .view_base import View

MODE_BROWSER = 16

# Browser attributes for the top level, in the order Live's sidebar shows.
ROOTS = ("sounds", "drums", "instruments", "audio_effects", "midi_effects", "max_for_live",
         "plugins", "clips", "samples", "grooves", "packs", "user_library",
         "current_project")


class Level(object):
    """One folder on the way down: its items and where the cursor is."""

    _count = 0

    def __init__(self, title, items):
        Level._count += 1
        self.key = Level._count     # new contents for the keyboard
        self.title = title
        self.items = items
        self.cursor = 0


def _flag(item, name):
    try:
        return bool(getattr(item, name))
    except Exception:
        return False


def _children(item):
    try:
        return list(item.children)
    except Exception:
        return []


class BrowserView(View):

    owns_list = True

    def __init__(self, song, display, browser):
        super(BrowserView, self).__init__(song, display)
        self._browser_fn = browser
        self._stack = []

    def browser(self):
        try:
            return self._browser_fn()
        except Exception:
            return None

    def available(self):
        return self.browser() is not None

    # -- model --------------------------------------------------------------

    def _roots(self):
        browser = self.browser()
        if browser is None:
            return []
        items = []
        for name in ROOTS:
            try:
                item = getattr(browser, name, None)
            except Exception:
                item = None
            if item is not None:
                items.append(item)
        try:
            items += list(browser.user_folders)
        except Exception:
            pass
        return items

    def _level(self):
        if not self._stack:
            self._stack.append(Level("Browser", self._roots()))
        return self._stack[-1]

    def _current(self):
        level = self._level()
        if not level.items:
            return None
        level.cursor = max(0, min(len(level.items) - 1, level.cursor))
        return level.items[level.cursor]

    # -- input --------------------------------------------------------------

    def move(self, delta):
        level = self._level()
        if level.items:
            level.cursor = max(0, min(len(level.items) - 1, level.cursor + delta))

    def open(self):
        item = self._current()
        children = _children(item) if item is not None else []
        if not children:
            return False
        self._stack.append(Level(lu.safe_name(item), children))
        return True

    def back(self):
        if len(self._stack) > 1:
            self._stack.pop()
            return True
        return False

    def activate(self):
        item = self._current()
        if item is None:
            return
        browser = self.browser()
        if _flag(item, "is_loadable") and browser is not None:
            if lu.attempt(browser.load_item, item):
                self._display.notify("Loaded", lu.safe_name(item))
        else:
            self.open()

    def encoder(self, delta, shift):
        self.move(delta)

    def press(self, control, shift):
        rows = self._display.list_rows()
        actions = {
            p.CTL_NAV_UP: lambda: self.move(-1),
            p.CTL_NAV_DOWN: lambda: self.move(1),
            p.CTL_NAV_LEFT: self.back,
            p.CTL_NAV_RIGHT: self.open,
            p.CTL_NAV_PUSH: self.activate,
            p.CTL_PRESET_UP: lambda: self.move(-rows),
            p.CTL_PRESET_DOWN: lambda: self.move(rows),
        }
        action = actions.get(control)
        if action is None:
            return False
        action()
        return True

    # -- output -------------------------------------------------------------

    def _row(self, level, index):
        item = level.items[index]
        return lu.safe_name(item), p.ROW_FOLDER if _flag(item, "is_folder") else 0

    def render(self, shift):
        d = self._display
        self.render_header()
        level = self._level()
        self._current()   # clamps the cursor
        # One PAGE for the whole mode: the LIST title names the folder.
        d.page(self.mode, "Browser", "", identity=(MODE_BROWSER,))
        for knob in range(8):
            d.empty_knob(knob)
        d.list(level.key, level.title, len(level.items), level.cursor,
               lambda i: self._row(level, i))

        item = self._current()
        count = len(level.items)
        dim = lambda ok: p.LIGHT_DIM if ok else p.LIGHT_OFF
        self.render_nav_lights(up=level.cursor > 0, down=level.cursor < count - 1,
                               left=len(self._stack) > 1,
                               right=_flag(item, "is_folder"),
                               push=dim(item is not None))
        d.light(p.CTL_PRESET_UP, dim(level.cursor > 0))
        d.light(p.CTL_PRESET_DOWN, dim(level.cursor < count - 1))
