#pragma once
/* The light guide: an RGB light above each key (CAP_LIGHT_GUIDE, BOARD_GUIDE_KEYS). */
#include <stdint.h>

void lightguide_tick_ms(uint32_t ms);   /* main loop: recolours when something changed */

/* the colour of a key, 0..255 per channel (pure, host-tested): with a scale on, the root
   blue, scale notes dim white, other notes off; without, a faint blue; a held key white */
void lightguide_colour(int note, int root, uint16_t mask, int scale_on, int held, uint8_t rgb[3]);
