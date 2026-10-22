#pragma once
#include <stdint.h>

void settings_load(void);                  /* newest saved settings, or defaults */
void settings_touch(void);                 /* schedule a save */
void settings_tick_ms(uint32_t ms, int idle);  /* saves only while idle (flash stalls the CPU) */
int settings_save_now(void);               /* 0 = ok */
int settings_pending(void);

/* 8 user presets (whole settings snapshots); 0 = ok */
int presets_load(int slot);
int presets_save(int slot);
int presets_used(int slot);
