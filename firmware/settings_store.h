#pragma once
#include <stdint.h>

void settings_load(void);           /* newest saved settings, or defaults */
void settings_touch(void);          /* schedule a save 3 s from now */
void settings_tick_ms(uint32_t ms);
void settings_save_now(void);

/* 8 user presets (whole settings snapshots); return 0 on success */
int presets_load(int slot);
int presets_save(int slot);
int presets_used(int slot);
