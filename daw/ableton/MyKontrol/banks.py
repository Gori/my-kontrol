# Parameter banks: which 8 parameters of a device go on the knobs, in pages.
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Order of preference, the same as Live's own control surface scripts:
#
#   1. Max for Live devices that define banks (get_bank_count() > 0)
#   2. Live's bank definitions for built-in devices (Operator, Wavetable,
#      Drift, Simpler, Analog, racks...): musically ordered pages with
#      names like "Oscillators" or "Filter Envelopes". surface.py passes in
#      ableton.v3's BANK_DEFINITIONS; this module never imports Live.
#   3. Everything else (plug-ins): 8 at a time in parameter order,
#      skipping "Device On".
#
# A definition slot is either a parameter name or one of Live's
# ParameterSlotDescription objects ("use X if parameter Y has value Z,
# else W"). We evaluate those ourselves from their recorded conditions
# instead of calling set_parameter_host(), because the definition objects
# are shared module state inside Live.

import re

BANK_PARAMETERS_KEY = "Parameters"
MAIN_BANK = "Main"
MAX_MAIN_BANK_INDEX = -1
BANK_SIZE = 8

RACK_CLASSES = ("InstrumentGroupDevice", "DrumGroupDevice",
                "AudioEffectGroupDevice", "MidiEffectGroupDevice")

# Used when Live's tables are not available (tests, older Live).
_RACK_BANKS = (
    ("Macros", ["Macro %d" % i for i in range(1, 9)]),
    ("Macros 2", ["Macro %d" % i for i in range(9, 17)]),
)


def is_generated_name(name):
    """True for the "Bank 3" names made up here when Live gives none."""
    return re.match(r"^Bank \d+$", name or "") is not None


def _str(value):
    try:
        return str(value)
    except Exception:
        return ""


def _find(device, name):
    for param in device.parameters:
        if getattr(param, "original_name", param.name) == name:
            return param
    return None


def resolve_slot(slot, device):
    """Parameter name for one definition slot ('' = empty slot)."""
    if slot is None:
        return ""
    if isinstance(slot, str):
        return slot
    conditions = getattr(slot, "_conditions", None)
    default = getattr(slot, "_default_parameter_name", None)
    if conditions is None or default is None:
        return _str(slot)
    for condition in conditions:
        result = True
        for sub in condition.get("ConditionsListName", ()):
            predicate = sub.get("Predicate")
            try:
                value = bool(predicate(_find(device, sub.get("ConditionName")))) if predicate else False
            except Exception:
                value = False
            if sub.get("Operand") == "or":
                result = result or value
            else:
                result = result and value
        if result:
            return condition.get("ResultingName") or ""
    return default


class Banks(object):

    def __init__(self, definitions=None):
        self._definitions = definitions if definitions is not None else {}

    # -- device kinds -------------------------------------------------------

    @staticmethod
    def _max_bank_count(device):
        try:
            return int(device.get_bank_count())
        except Exception:
            return 0

    @staticmethod
    def _max_has_main(device):
        try:
            return bool(device.get_bank_parameters(MAX_MAIN_BANK_INDEX))
        except Exception:
            return False

    def _definition(self, device):
        """[(bank name, [slots])] from Live's tables, or None."""
        class_name = getattr(device, "class_name", "")
        definition = self._definitions.get(class_name)
        if definition is not None:
            try:
                return [(name, list(bank.get(BANK_PARAMETERS_KEY) or ()))
                        for name, bank in definition.items()]
            except Exception:
                return None
        if class_name in RACK_CLASSES:
            return [(name, list(slots)) for name, slots in _RACK_BANKS]
        return None

    @staticmethod
    def _visible_macro_banks(device, banks):
        count = getattr(device, "visible_macro_count", None)
        if count is None or getattr(device, "class_name", "") not in RACK_CLASSES:
            return banks
        return banks[:max(1, (int(count) + BANK_SIZE - 1) // BANK_SIZE)]

    # -- public -------------------------------------------------------------

    def names(self, device):
        if device is None:
            return []
        mx = self._max_bank_count(device)
        if mx:
            main = int(self._max_has_main(device))
            names = []
            for i in range(mx + main):
                try:
                    name = device.get_bank_name(i - main)
                except Exception:
                    name = ""
                names.append(name or (MAIN_BANK if main and i == 0 else "Bank %d" % (i + 1)))
            return names
        definition = self._definition(device)
        if definition is not None:
            return [name for name, _ in self._visible_macro_banks(device, definition)]
        count = (len(list(device.parameters)[1:]) + BANK_SIZE - 1) // BANK_SIZE
        return ["Bank %d" % (i + 1) for i in range(count)]

    def parameters(self, device, index):
        """The 8 parameters (or None) of bank `index`."""
        empty = [None] * BANK_SIZE
        if device is None or index < 0:
            return empty
        params = list(device.parameters)
        mx = self._max_bank_count(device)
        if mx:
            try:
                indices = device.get_bank_parameters(index - int(self._max_has_main(device)))
                out = [params[i] if 0 <= i < len(params) else None for i in indices]
            except Exception:
                out = []
            return (out + empty)[:BANK_SIZE]
        definition = self._definition(device)
        if definition is not None:
            definition = self._visible_macro_banks(device, definition)
            if index >= len(definition):
                return empty
            by_name = {}
            for param in params:
                by_name.setdefault(getattr(param, "original_name", param.name), param)
            out = [by_name.get(resolve_slot(slot, device)) for slot in definition[index][1]]
            return (out + empty)[:BANK_SIZE]
        chunk = params[1:][index * BANK_SIZE:(index + 1) * BANK_SIZE]
        return (chunk + empty)[:BANK_SIZE]
