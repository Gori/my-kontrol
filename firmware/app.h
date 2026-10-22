#pragma once
#include <stdint.h>

void app_init(void);
void app_poll(void);
enum { HW_LEDS = 1, HW_TOUCH = 2, HW_PITCH = 4, HW_MOD = 8 };
extern uint8_t hw_status;            /* which peripherals answered at boot */
void app_key(uint8_t key, uint8_t velocity, int on);  /* key = MIDI note before octave/transpose */
