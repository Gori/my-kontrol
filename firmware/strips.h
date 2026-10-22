#pragma once
/* Pitch and mod touch strips: one touch controller each on its own bit-banged
   I2C bus (re/hardware_map.md 4). Position 0 = untouched, 1..200 along the strip. */
#include <stdint.h>

enum { STRIP_PITCH, STRIP_MOD };

int strips_init(void);                     /* bit0 pitch ok, bit1 mod ok */
int strips_read(int strip, uint8_t *pos);  /* 0 on success */
extern uint8_t strips_probe_raw[2][3];     /* first read after init, for diagnostics */
