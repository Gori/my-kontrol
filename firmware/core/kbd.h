#pragma once
/* USB keyboard output: the 4D encoder navigates the focused DAW window. */
#include <stdint.h>

enum { KMOD_CTRL = 0x01, KMOD_SHIFT = 0x02, KMOD_ALT = 0x04, KMOD_CMD = 0x08 };
enum { KEY_Q = 0x14, KEY_R = 0x15, KEY_ENTER = 0x28, KEY_SPACE = 0x2C, KEY_MINUS = 0x2D, KEY_EQUAL = 0x2E,
       KEY_RIGHT = 0x4F, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52 };

void kbd_tap(uint8_t modifier, uint8_t key);   /* queued press + release */
