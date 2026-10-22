#pragma once
/* Pedal jack: TRS on PC4/PC5 (ADC ch14/15), plug detect on PB12.
   Each contact is sampled while the other is driven high (docs/hardware/m32.md 6).
   Two footswitches; released level is learned while the jack settles. */
#include <stdint.h>

void pedal_init(void);
/* call every 10 ms; returns 1 with *sw (0 tip, 1 ring) and *down on a change */
int pedal_poll(uint8_t *sw, uint8_t *down);
extern uint16_t pedal_raw[3];   /* plug detect pin, contact 1, contact 2 */
