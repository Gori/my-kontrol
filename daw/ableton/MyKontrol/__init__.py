# My Kontrol: Ableton Live remote script for keyboards running the My Kontrol
# firmware (first board: Komplete Kontrol M32).
#
# Copyright (C) 2026 The My Kontrol contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version. See https://www.gnu.org/licenses/gpl-3.0.html
#
# Live-only imports happen inside the functions below, so the pure-Python
# modules of this package (protocol, outbox, session, engine, views) can be
# imported and unit tested outside Live.

VENDOR_ID = 0x17CC
PRODUCT_ID = 0x1860
MODEL_NAMES = ["My Kontrol M32", "My Kontrol"]


def get_capabilities():
    """Lets Live pick this script and its ports automatically when the
    keyboard is plugged in. USB MIDI cable 1 is the Keys port (track
    input), cable 2 the DAW port (this script)."""
    from ableton.v2.control_surface.capabilities import (
        AUTO_LOAD_KEY, CONTROLLER_ID_KEY, NOTES_CC, PORTS_KEY, SCRIPT,
        controller_id, inport, outport)
    return {
        CONTROLLER_ID_KEY: controller_id(vendor_id=VENDOR_ID,
                                         product_ids=[PRODUCT_ID],
                                         model_name=MODEL_NAMES),
        PORTS_KEY: [
            inport(props=[NOTES_CC]),     # My Kontrol Keys -> tracks
            inport(props=[SCRIPT]),       # My Kontrol DAW  -> script
            outport(props=[]),            # My Kontrol Keys (unused)
            outport(props=[SCRIPT]),      # script -> My Kontrol DAW
        ],
        AUTO_LOAD_KEY: True,
    }


def create_instance(c_instance):
    from .surface import MyKontrol
    return MyKontrol(c_instance)
