#pragma once
/* Knob-touch controller on bit-banged I2C bus C (PB10 SCL, PB11 SDA).
   Returns an 8-bit mask of touched knobs (docs/hardware/m32.md 3). */
#include <stdint.h>

int touch_init(void);            /* 0 if the chip answered */
int touch_poll(uint8_t *mask);   /* 1 when the debounced mask changed, -1 on a bus error */
