#pragma once
/* Logical controls shared by every keyboard. Boards translate their own
   matrix positions to these; the host protocol sends them as note numbers
   (docs/protocol.md), so the numbers are part of the protocol: only append. */

enum {
    CTL_SHIFT = 0,
    CTL_SCALE = 1,
    CTL_ARP = 2,
    CTL_UNDO = 3,
    CTL_QUANTIZE = 4,
    CTL_IDEAS = 5,
    CTL_LOOP = 6,
    CTL_METRO = 7,
    CTL_TEMPO = 8,
    CTL_PLAY = 9,
    CTL_REC = 10,
    CTL_STOP = 11,
    CTL_PRESET_UP = 12,
    CTL_PRESET_DOWN = 13,
    CTL_MUTE = 14,
    CTL_SOLO = 15,
    CTL_BROWSER = 16,
    CTL_PLUGIN = 17,
    CTL_TRACK = 18,
    CTL_OCT_DOWN = 19,
    CTL_OCT_UP = 20,
    CTL_NAV_UP = 21,
    CTL_NAV_LEFT = 22,
    CTL_NAV_RIGHT = 23,
    CTL_NAV_DOWN = 24,
    CTL_NAV_PUSH = 25,
    CTL_PEDAL_PLUG = 26,     /* state, not a button */

    /* 32..63: controls on other Komplete Kontrol models */
    CTL_CLEAR = 32,
    CTL_MIXER = 33,
    CTL_INSTANCE = 34,
    CTL_MIDI = 35,
    CTL_SETUP = 36,
    CTL_AUTO = 37,
    CTL_COUNT_IN = 38,
    CTL_PAGE_LEFT = 39,
    CTL_PAGE_RIGHT = 40,
    CTL_FIXED_VEL = 41,
    CTL_KEYS = 42,
    CTL_CHORD = 43,

    CTL_COUNT = 64,
};
