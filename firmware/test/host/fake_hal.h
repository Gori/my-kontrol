#pragma once
#include "hal.h"
#include "midi.h"

extern uint32_t fake_now_ms;
extern int fake_flash_fail_after;
extern uint8_t fake_lights[64];
extern int fake_display_on;
extern uint8_t fake_midi[PORT_COUNT][4096];   /* [PORT_DIN]: the 5-pin byte stream */
extern int fake_midi_n[PORT_COUNT];

void fake_reset(void);
void fake_drain_midi(void);
void fake_clear_midi(void);
