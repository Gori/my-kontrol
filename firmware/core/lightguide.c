/* The light guide (lightguide.h). Built into every image; only boards that set
   BOARD_GUIDE_KEYS call it (app.c), so it costs other boards nothing. */
#include "lightguide.h"
#include "hal.h"
#include "play.h"
#include "settings.h"
#include "smartplay.h"

#ifndef BOARD_GUIDE_KEYS
#define BOARD_GUIDE_KEYS 0
#endif

void lightguide_colour(int note, int root, uint16_t mask, int scale_on, int held, uint8_t rgb[3])
{
    uint8_t r = 0, g = 0, b = 0;
    if (held) {
        r = g = b = 255;
    } else if (scale_on) {
        int pc = ((note - root) % 12 + 12) % 12;
        if (pc == 0)
            b = 255, g = 64;
        else if (mask >> pc & 1)
            r = g = b = 48;
    } else {
        b = 24;
    }
    rgb[0] = r, rgb[1] = g, rgb[2] = b;
}

#if BOARD_GUIDE_KEYS
static uint8_t shown[BOARD_GUIDE_KEYS][3];
static uint8_t have;
static uint32_t since;

void lightguide_tick_ms(uint32_t ms)
{
    since += ms;
    if (since < 20)                    /* at most 50 updates a second */
        return;
    since = 0;
    int scale_on = settings.sp.scale_mode != SCALE_OFF, root = sp_scale_root(), changed = 0;
    uint16_t mask = sp_scale_mask();
    for (int k = 0; k < BOARD_GUIDE_KEYS; k++) {
        int note = board_info.base_note + k + settings.transpose + 12 * settings.octave;
        uint8_t c[3];
        lightguide_colour(note, root, mask, scale_on, play_key_down(k), c);
        if (!have || c[0] != shown[k][0] || c[1] != shown[k][1] || c[2] != shown[k][2]) {
            shown[k][0] = c[0], shown[k][1] = c[1], shown[k][2] = c[2];
            board_key_light((uint8_t)k, c[0], c[1], c[2]);
            changed = 1;
        }
    }
    have = 1;
    if (changed)
        board_key_lights_flush();
}
#endif
