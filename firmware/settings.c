#include "settings.h"

settings_t settings;

void settings_defaults(settings_t *s)
{
    sp_defaults(&s->sp);
    s->channel = 0;
    s->octave = 0;
    s->transpose = 0;
    s->vel_curve = VEL_LINEAR;
    s->fixed_velocity = 100;
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 8; i++)
            s->knob_cc[p][i] = 14 + 8 * p + i;  /* page 1 = stock CCs 14..21 */
    s->knob_page = 0;
    s->pedal_cc[0] = 64;         /* sustain */
    s->pedal_cc[1] = 65;         /* portamento / second switch, stock default */
    s->strip_mod_cc = 1;
    s->daw_mode = 0;
    s->contrast = 0xE0;          /* NI's init value */
}
