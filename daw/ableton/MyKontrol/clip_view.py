# Clip view: an 8 x 4 window onto Session View with a cursor.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   Knobs                 volume of the 8 grid tracks
#   4D turn               move the cursor through scenes (scrolls the window)
#   4D left/right/up/down move the cursor; past the edge the window scrolls
#   4D push               launch the clip under the cursor (Shift: its scene)
#   Preset up / down      page the window 4 scenes up / down
#
# The window is shown in Live as the coloured session ring. Moving the
# cursor selects that track and scene in Live, so Quantize, Mute and Solo
# act on what the cursor points at.

from . import protocol as p
from . import liveutil as lu
from .view_base import View

COLS = p.GRID_COLS
ROWS = p.GRID_ROWS


def _slot(track, scene_index):
    try:
        return track.clip_slots[scene_index]
    except (IndexError, AttributeError, TypeError):
        return None


class ClipView(View):

    def __init__(self, song, display, highlight=None):
        super(ClipView, self).__init__(song, display)
        self._highlight = highlight or (lambda *a: None)
        self._shown_ring = None
        self.track_offset = 0
        self.scene_offset = 0
        self.col = 0     # cursor, relative to the window
        self.row = 0

    # -- model --------------------------------------------------------------

    def _tracks(self):
        return list(self._song.visible_tracks)

    def _scenes(self):
        return list(self._song.scenes)

    def _clamp(self, tracks, scenes):
        """Keep window and cursor valid after tracks/scenes were deleted."""
        abs_col = max(0, min(len(tracks) - 1, self.track_offset + self.col))
        abs_row = max(0, min(len(scenes) - 1, self.scene_offset + self.row))
        self._show(abs_col, abs_row)

    def _show(self, abs_col, abs_row):
        """Put the cursor on an absolute slot, scrolling as little as possible."""
        if abs_col < self.track_offset:
            self.track_offset = abs_col
        elif abs_col >= self.track_offset + COLS:
            self.track_offset = abs_col - COLS + 1
        if abs_row < self.scene_offset:
            self.scene_offset = abs_row
        elif abs_row >= self.scene_offset + ROWS:
            self.scene_offset = abs_row - ROWS + 1
        self.track_offset = max(0, self.track_offset)
        self.scene_offset = max(0, self.scene_offset)
        self.col = max(0, abs_col - self.track_offset)
        self.row = max(0, abs_row - self.scene_offset)

    def _window(self):
        tracks, scenes = self._tracks(), self._scenes()
        self._clamp(tracks, scenes)
        cols = [tracks[i] if i < len(tracks) else None
                for i in range(self.track_offset, self.track_offset + COLS)]
        rows = [scenes[i] if i < len(scenes) else None
                for i in range(self.scene_offset, self.scene_offset + ROWS)]
        return tracks, scenes, cols, rows

    def _cursor_slot(self):
        tracks, scenes, cols, rows = self._window()
        track, scene = cols[self.col], rows[self.row]
        if track is None or scene is None:
            return track, scene, None
        return track, scene, _slot(track, self.scene_offset + self.row)

    def knob_parameter(self, knob, shift):
        track = self._window()[2][knob]
        return lu.volume(track) if track is not None else None

    # -- input --------------------------------------------------------------

    def enter(self):
        super(ClipView, self).enter()
        # Start where Live's selection is.
        song = self._song
        col = lu.index_of(self._tracks(), lu.selected_track(song))
        row = lu.index_of(self._scenes(), song.view.selected_scene)
        if col >= 0 or row >= 0:
            self._show(col if col >= 0 else self.track_offset + self.col,
                       row if row >= 0 else self.scene_offset + self.row)

    def exit(self):
        super(ClipView, self).exit()
        self._set_ring(None)

    def move(self, dcol, drow):
        tracks, scenes = self._tracks(), self._scenes()
        if not tracks or not scenes:
            return
        abs_col = max(0, min(len(tracks) - 1, self.track_offset + self.col + dcol))
        abs_row = max(0, min(len(scenes) - 1, self.scene_offset + self.row + drow))
        self._show(abs_col, abs_row)
        self._select_in_live(tracks[abs_col], scenes[abs_row])

    def page(self, direction):
        tracks, scenes = self._tracks(), self._scenes()
        if not scenes:
            return
        top = max(0, min(max(0, len(scenes) - ROWS), self.scene_offset + direction * ROWS))
        abs_row = max(0, min(len(scenes) - 1, top + self.row))
        self.scene_offset = top
        self.row = abs_row - top
        if tracks:
            self._clamp(tracks, scenes)
            self._select_in_live(tracks[self.track_offset + self.col], scenes[abs_row])

    def _select_in_live(self, track, scene):
        lu.set_attr(self._song.view, "selected_track", track)
        lu.set_attr(self._song.view, "selected_scene", scene)

    def encoder(self, delta, shift):
        self.move(0, delta)

    def press(self, control, shift):
        moves = {p.CTL_NAV_LEFT: (-1, 0), p.CTL_NAV_RIGHT: (1, 0),
                 p.CTL_NAV_UP: (0, -1), p.CTL_NAV_DOWN: (0, 1)}
        if control in moves:
            self.move(*moves[control])
        elif control == p.CTL_NAV_PUSH:
            track, scene, slot = self._cursor_slot()
            if shift and scene is not None:
                lu.attempt(scene.fire)
            elif slot is not None:
                lu.attempt(slot.fire)
        elif control == p.CTL_PRESET_UP:
            self.page(-1)
        elif control == p.CTL_PRESET_DOWN:
            self.page(1)
        else:
            return False
        return True

    # -- output -------------------------------------------------------------

    def _set_ring(self, ring):
        if ring != self._shown_ring:
            self._shown_ring = ring
            if ring is None:
                self._highlight(-1, -1, -1, -1, False)
            else:
                self._highlight(ring[0], ring[1], COLS, ROWS, False)

    def render(self, shift):
        d = self._display
        self.render_header()
        tracks, scenes, cols, rows = self._window()

        first, last = self.track_offset + 1, min(self.track_offset + COLS, len(tracks))
        top, bottom = self.scene_offset + 1, min(self.scene_offset + ROWS, len(scenes))
        d.text(p.TEXT_BANK, "")
        d.text(p.TEXT_SUBTITLE, "Tracks %d-%d Scenes %d-%d" % (first, last, top, bottom))

        states = []
        for r, scene in enumerate(rows):
            for c, track in enumerate(cols):
                slot = None
                if track is not None and scene is not None:
                    slot = _slot(track, self.scene_offset + r)
                states.append(lu.slot_state(track, slot))
        d.grid(COLS, ROWS, self.col, self.row, states)

        for c, track in enumerate(cols):
            d.text(p.TEXT_COLUMN + c, lu.safe_name(track))
            if track is None:
                d.empty_knob(c)
            else:
                lu.show_parameter(d, c, lu.volume(track), label=lu.safe_name(track))
        for r, scene in enumerate(rows):
            if scene is None:
                name = ""
            else:
                name = lu.safe_name(scene) or str(self.scene_offset + r + 1)
            d.text(p.TEXT_ROW + r, name)

        abs_col, abs_row = self.track_offset + self.col, self.scene_offset + self.row
        dim = lambda ok: p.LIGHT_DIM if ok else p.LIGHT_OFF
        self.render_nav_lights(up=abs_row > 0,
                               down=abs_row < len(scenes) - 1,
                               left=abs_col > 0,
                               right=abs_col < len(tracks) - 1,
                               push=dim(bool(tracks) and bool(scenes)))
        d.light(p.CTL_PRESET_UP, dim(self.scene_offset > 0))
        d.light(p.CTL_PRESET_DOWN, dim(self.scene_offset + ROWS < len(scenes)))
        self._set_ring((self.track_offset, self.scene_offset) if tracks and scenes else None)
