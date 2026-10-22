# Parameter bank tests: Live-style definitions, conditional slots, Max for
# Live banks, racks and the plain fallback.
# SPDX-License-Identifier: GPL-3.0-or-later

import unittest

from fakes import FakeDevice, FakeParam, FakeRack
from MyKontrol.banks import Banks, resolve_slot


class FakeSlot(object):
    """Shaped like Live's ParameterSlotDescription after
    use("A").if_parameter("Sync").has_value("Hertz").else_use("B")."""

    def __init__(self, if_name, cond_param, cond_value, else_name):
        self._default_parameter_name = else_name
        self._conditions = [{
            "ResultingName": if_name,
            "DisplayNameTransformer": None,
            "ConditionsListName": [{
                "ConditionName": cond_param, "Operand": "and",
                "Predicate": lambda p: str(p) == cond_value}]}]


def device(class_name, names):
    return FakeDevice("Dev", class_name=class_name, param_names=names)


class Definitions(unittest.TestCase):

    def setUp(self):
        self.defs = {"Synth": dict([
            ("Main", {"Parameters": ("Cutoff", "Res", "", "Volume")}),
            ("LFO", {"Parameters": (FakeSlot("Rate Hz", "Sync", "Off", "Rate Sync"),)}),
        ])}
        self.banks = Banks(self.defs)
        self.dev = device("Synth", ["Volume", "Res", "Cutoff", "Sync", "Rate Hz", "Rate Sync"])

    def test_names_and_order(self):
        self.assertEqual(self.banks.names(self.dev), ["Main", "LFO"])
        names = [p.name if p else None for p in self.banks.parameters(self.dev, 0)]
        self.assertEqual(names, ["Cutoff", "Res", None, "Volume", None, None, None, None])

    def test_conditional_slot(self):
        sync = self.dev.parameters[4]
        sync.is_quantized, sync.max, sync.value = True, 1, 0         # str -> "Off"
        self.assertEqual(self.banks.parameters(self.dev, 1)[0].name, "Rate Hz")
        sync.value = 1                                                # "On"
        self.assertEqual(self.banks.parameters(self.dev, 1)[0].name, "Rate Sync")

    def test_plain_names_resolve_as_is(self):
        self.assertEqual(resolve_slot("Cutoff", self.dev), "Cutoff")
        self.assertEqual(resolve_slot(None, self.dev), "")

    def test_out_of_range(self):
        self.assertEqual(self.banks.parameters(self.dev, 9), [None] * 8)


class Fallbacks(unittest.TestCase):

    def test_plugin_is_8_at_a_time(self):
        dev = FakeDevice("VST", n_params=19)
        banks = Banks({})
        self.assertEqual(banks.names(dev), ["Bank 1", "Bank 2", "Bank 3"])
        self.assertEqual(banks.parameters(dev, 2)[2].name, "VST P19")
        self.assertIsNone(banks.parameters(dev, 2)[3])

    def test_rack_macros_respect_visible_count(self):
        rack = FakeRack("Rack", [])
        banks = Banks({})
        self.assertEqual(banks.names(rack), ["Macros", "Macros 2"])
        rack.visible_macro_count = 8
        self.assertEqual(banks.names(rack), ["Macros"])
        self.assertEqual(banks.parameters(rack, 0)[7].name, "Macro 8")

    def test_max_for_live_banks(self):
        dev = FakeDevice("M4L", n_params=10)
        dev.get_bank_count = lambda: 2
        dev.get_bank_name = lambda i: {-1: "", 0: "Osc", 1: ""}[i]
        dev.get_bank_parameters = lambda i: {-1: [3, 2, -1, -1, -1, -1, -1, -1],
                                             0: [1, 2, 3, 4, 5, 6, 7, 8],
                                             1: [9, 10, -1, -1, -1, -1, -1, -1]}[i]
        banks = Banks({})
        self.assertEqual(banks.names(dev), ["Main", "Osc", "Bank 3"])
        self.assertEqual(banks.parameters(dev, 0)[0].name, "M4L P3")
        self.assertEqual(banks.parameters(dev, 2)[1].name, "M4L P10")
        self.assertIsNone(banks.parameters(dev, 2)[2])

    def test_value_text_prefers_str_for_value(self):
        from MyKontrol import liveutil as lu
        param = FakeParam("Pan", 0.0, -1.0, 1.0)
        param.str_for_value = lambda v: "C"
        self.assertEqual(lu.parameter_text(param), "C")
        self.assertTrue(lu.parameter_flags(param) & 2)    # bipolar


if __name__ == "__main__":
    unittest.main()
