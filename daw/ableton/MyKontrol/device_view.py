# Instrument and Plug-in views: the 8 knobs control a parameter bank.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Instrument view (VIEW 5): the selected track's instrument. If the track's
# first instrument is an Instrument or Drum Rack, bank 1 is the rack's
# macros and the following banks belong to the instrument playing inside it
# (the selected chain / drum pad first, searching nested racks).
#
# Plug-in view (VIEW 0): Live's selected device on the selected track.
# Shift + Preset up / down selects the previous / next device in its chain.
#
#   Knobs                 8 parameters of the current bank
#   Preset up / down      previous / next bank
#   4D turn, left/right   select track          (only with "4D as Keys" off)
#   4D up / down          Plug-in: previous / next device; Instrument: bank
#   4D push               device on / off
#
# Title fields: TEXT_DEVICE = the device whose bank is shown, TEXT_BANK =
# bank name, TEXT_SUBTITLE = bank position ("2/6"). The bank is remembered
# per device (Plug-in) or per track (Instrument).

from . import protocol as p
from . import liveutil as lu
from .banks import Banks
from .view_base import View


class BankedView(View):

    def __init__(self, song, display, banks=None):
        super(BankedView, self).__init__(song, display)
        self._banks = banks or Banks()
        self._bank_of = {}     # memory key -> bank index

    # -- to override --------------------------------------------------------

    def bank_list(self):
        """[(owner device, bank index in owner, bank name)] and memory key."""
        raise NotImplementedError

    def target(self):
        """Device toggled by 4D push, or None."""
        raise NotImplementedError

    # -- model --------------------------------------------------------------

    def _current(self):
        banks, key = self.bank_list()
        index = self._bank_of.get(key, 0)
        index = max(0, min(len(banks) - 1, index)) if banks else 0
        if not banks:
            return banks, key, 0, None, [None] * 8
        owner, owner_index, _ = banks[index]
        try:
            params = list(self._banks.parameters(owner, owner_index))
        except Exception:
            params = []
        return banks, key, index, owner, (params + [None] * 8)[:8]

    def knob_parameter(self, knob, shift):
        return self._current()[4][knob]

    def step_bank(self, delta):
        banks, key, index, _, _ = self._current()
        new = max(0, min(len(banks) - 1, index + delta)) if banks else 0
        if key is not None and new != index:
            self._bank_of[key] = new
            self._nudger.reset()
            return True
        return False

    # -- input --------------------------------------------------------------

    def encoder(self, delta, shift):
        lu.select_relative_track(self._song, delta)

    def press(self, control, shift):
        if control == p.CTL_NAV_LEFT:
            lu.select_relative_track(self._song, -1)
        elif control == p.CTL_NAV_RIGHT:
            lu.select_relative_track(self._song, 1)
        elif control == p.CTL_NAV_PUSH:
            device = self.target()
            state = lu.toggle_device(device)
            if state is not None:
                self._display.notify(lu.safe_name(device), "On" if state else "Off")
        elif control in (p.CTL_PRESET_UP, p.CTL_PRESET_DOWN):
            self.preset(-1 if control == p.CTL_PRESET_UP else 1, shift)
        else:
            return False
        return True

    def preset(self, delta, shift):
        if not shift:
            self.step_bank(delta)

    # -- output -------------------------------------------------------------

    def empty_text(self):
        return ""

    def render(self, shift):
        d = self._display
        banks, _, index, owner, params = self._current()
        d.text(p.TEXT_TRACK, lu.safe_name(lu.selected_track(self._song)))
        d.text(p.TEXT_DEVICE, lu.safe_name(owner) if owner is not None
               else lu.safe_name(self.target()))
        if banks:
            d.text(p.TEXT_BANK, banks[index][2])
            d.text(p.TEXT_SUBTITLE, "%d/%d" % (index + 1, len(banks)))
        else:
            d.text(p.TEXT_BANK, self.empty_text())
            d.text(p.TEXT_SUBTITLE, "")
        for knob, param in enumerate(params):
            lu.show_parameter(d, knob, param)

        target = self.target()
        on = lu.device_on_parameter(target)
        push = p.LIGHT_OFF if on is None else (p.LIGHT_ON if on.value > on.min else p.LIGHT_DIM)
        dim = lambda ok: p.LIGHT_DIM if ok else p.LIGHT_OFF
        up, down = self.up_down_available(index, len(banks))
        self.render_nav_lights(up=up, down=down,
                               left=lu.neighbour_exists(self._song, -1),
                               right=lu.neighbour_exists(self._song, 1),
                               push=push)
        prev_ok, next_ok = self.preset_available(index, len(banks), shift)
        d.light(p.CTL_PRESET_UP, dim(prev_ok))
        d.light(p.CTL_PRESET_DOWN, dim(next_ok))

    def up_down_available(self, index, count):
        return index > 0, index < count - 1

    def preset_available(self, index, count, shift):
        if shift:
            return False, False
        return index > 0, index < count - 1


class PluginView(BankedView):
    """Live's selected device."""

    def target(self):
        return lu.selected_device(self._song)

    def bank_list(self):
        device = self.target()
        if device is None:
            return [], None
        try:
            names = list(self._banks.names(device))
        except Exception:
            names = []
        return [(device, i, name) for i, name in enumerate(names)], ("device", lu.live_id(device))

    def press(self, control, shift):
        if control == p.CTL_NAV_UP:
            lu.select_relative_device(self._song, -1)
            return True
        if control == p.CTL_NAV_DOWN:
            lu.select_relative_device(self._song, 1)
            return True
        return super(PluginView, self).press(control, shift)

    def preset(self, delta, shift):
        if shift:
            lu.select_relative_device(self._song, delta)
        else:
            self.step_bank(delta)

    def empty_text(self):
        return "No device"

    def up_down_available(self, index, count):
        return lu.device_neighbour_exists(self._song, -1), lu.device_neighbour_exists(self._song, 1)

    def preset_available(self, index, count, shift):
        if shift:
            return self.up_down_available(index, count)
        return index > 0, index < count - 1


class InstrumentView(BankedView):
    """The selected track's instrument (rack macros first)."""

    def _instrument(self):
        return lu.track_instrument(lu.selected_track(self._song))

    def target(self):
        rack, instrument = self._instrument()
        return rack if rack is not None else instrument

    def bank_list(self):
        track = lu.selected_track(self._song)
        rack, instrument = self._instrument()
        banks = []
        if rack is not None:
            try:
                macro_names = list(self._banks.names(rack))
            except Exception:
                macro_names = []
            banks.append((rack, 0, macro_names[0] if macro_names else "Macros"))
        if instrument is not None:
            try:
                names = list(self._banks.names(instrument))
            except Exception:
                names = []
            banks += [(instrument, i, name) for i, name in enumerate(names)]
        key = ("track", lu.live_id(track)) if track is not None else None
        return banks, key

    def press(self, control, shift):
        if control in (p.CTL_NAV_UP, p.CTL_NAV_DOWN):
            self.step_bank(-1 if control == p.CTL_NAV_UP else 1)
            return True
        return super(InstrumentView, self).press(control, shift)

    def empty_text(self):
        return "No instrument"
