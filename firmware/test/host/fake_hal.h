#pragma once
#include "hal.h"

extern uint32_t fake_now_ms;
extern int fake_flash_fail_after;
extern uint8_t fake_lights[64];
extern int fake_display_on;
extern uint8_t fake_midi[2][4096];
extern int fake_midi_n[2];

void fake_reset(void);
void fake_drain_midi(void);
void fake_clear_midi(void);
