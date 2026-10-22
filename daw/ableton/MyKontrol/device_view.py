# Device view: the 8 knobs control a parameter bank of the selected device.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   Knobs                 8 parameters of the current bank
#   4D turn               select track
#   4D left / right       select track
#   4D up / down          previous / next device
#   4D push               device on / off
#   Preset up / down      previous / next parameter bank
#   Shift + Preset        previous / next device
#
# The bank is remembered per device, so going back to a device returns to
# the bank you left it on.

from . import protocol as p
from . import liveutil as lu
from .view_base import View


class DeviceView(View):

    def __init__(self, song, display, banks):
        super(DeviceView, self).__init__(song, display)
        self._banks = banks
        self._bank_of = {}   # live_id(device) -> bank index

    # -- model --------------------------------------------------------------

    def _device(self):
        return lu.selected_device(self._song)

    def _bank_list(self, device):
        if device is None:
            return [], []
        try:
            banks = [list(b) for b in self._banks.banks(device)]
            names = list(self._banks.names(device))
        except Exception:
            banks, names = [], []
        names += ["Bank %d" % (i + 1) for i in range(len(names), len(banks))]
        return banks, names

    def _bank_index(self, device, count):
        index = self._bank_of.get(lu.live_id(device), 0)
        return max(0, min(count - 1, index)) if count else 0

    def _current_bank(self):
        device = self._device()
        banks, names = self._bank_list(device)
        index = self._bank_index(device, len(banks))
        params = banks[index] if banks else []
        params = (params + [None] * 8)[:8]
        return device, index, banks, names, params

    def knob_parameter(self, knob, shift):
        return self._current_bank()[4][knob]

    # -- input --------------------------------------------------------------

    def _step_bank(self, delta):
        device, index, banks, _, _ = self._current_bank()
        new = max(0, min(len(banks) - 1, index + delta)) if banks else 0
        if device is not None and new != index:
            self._bank_of[lu.live_id(device)] = new
            self._nudger.reset()

    def encoder(self, delta, shift):
        lu.select_relative_track(self._song, delta)

    def press(self, control, shift):
        song = self._song
        if control == p.CTL_NAV_LEFT:
            lu.select_relative_track(song, -1)
        elif control == p.CTL_NAV_RIGHT:
            lu.select_relative_track(song, 1)
        elif control == p.CTL_NAV_UP:
            lu.select_relative_device(song, -1)
        elif control == p.CTL_NAV_DOWN:
            lu.select_relative_device(song, 1)
        elif control == p.CTL_NAV_PUSH:
            device = self._device()
            state = lu.toggle_device(device)
            if state is not None:
                self._display.notify(lu.safe_name(device), "On" if state else "Off")
        elif control in (p.CTL_PRESET_UP, p.CTL_PRESET_DOWN):
            delta = -1 if control == p.CTL_PRESET_UP else 1
            if shift:
                lu.select_relative_device(song, delta)
            else:
                self._step_bank(delta)
        else:
            return False
        return True

    # -- output -------------------------------------------------------------

    def render(self, shift):
        d = self._display
        self.render_header()
        device, index, banks, names, params = self._current_bank()
        if banks:
            d.text(p.TEXT_BANK, names[index])
            d.text(p.TEXT_SUBTITLE, "Bank %d/%d" % (index + 1, len(banks)))
        else:
            d.text(p.TEXT_BANK, "")
            d.text(p.TEXT_SUBTITLE, "No device" if device is None else "")
        for knob, param in enumerate(params):
            lu.show_parameter(d, knob, param)

        song = self._song
        on = lu.device_on_parameter(device)
        if on is None:
            push = p.LIGHT_OFF
        else:
            push = p.LIGHT_ON if on.value > on.min else p.LIGHT_DIM
        self.render_nav_lights(up=lu.device_neighbour_exists(song, -1),
                               down=lu.device_neighbour_exists(song, 1),
                               left=lu.neighbour_exists(song, -1),
                               right=lu.neighbour_exists(song, 1),
                               push=push)
        dim = lambda ok: p.LIGHT_DIM if ok else p.LIGHT_OFF
        if shift:
            d.light(p.CTL_PRESET_UP, dim(lu.device_neighbour_exists(song, -1)))
            d.light(p.CTL_PRESET_DOWN, dim(lu.device_neighbour_exists(song, 1)))
        else:
            d.light(p.CTL_PRESET_UP, dim(index > 0))
            d.light(p.CTL_PRESET_DOWN, dim(index < len(banks) - 1))
