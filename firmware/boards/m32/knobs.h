#pragma once
/* 8 endless knobs: dual-wiper pots on ADC1 ch4-7 behind a 4:1 mux (PB8/PB9).
   See re/hardware_map.md 3. */
#include <stdint.h>

void knobs_init(void);
/* Call from the main loop; returns 1 and fills *knob / *delta when a knob moved. */
int knobs_poll(uint8_t *knob, int8_t *delta);
extern uint16_t knobs_raw[8][2];
uint16_t knobs_adc(int ch);   /* one 12-bit conversion, shared with the pedal */
