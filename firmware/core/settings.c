#include "settings.h"

settings_t settings;

const uint8_t settings_screensaver_min[] = {0, 2, 5, 10, 30};
const uint8_t settings_screensaver_count = sizeof settings_screensaver_min;
const char *const knob_target_names[KM_COUNT] = {"Off", "Instrument", "Plug-in", "Volume", "Pan", "Sends"};

void settings_defaults(settings_t *s)
{
    uint8_t *p = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++)
        p[i] = 0;
    sp_defaults(&s->sp);
    s->channel = 0;
    s->vel_curve = VEL_LINEAR;
    s->vel_curve_prev = VEL_LINEAR;
    s->fixed_velocity = 100;
    s->split_note = 60;
    s->lower_channel = 1;
    for (int p = 0; p < KNOB_PAGES; p++)
        for (int i = 0; i < BOARD_KNOBS; i++)
            s->knob_cc[p][i] = 14 + BOARD_KNOBS * p + i;   /* page 1 = NI's CCs 14..21 */
    s->knob_mode = KNOBS_DAW;
    for (int i = 0; i < KNOB_MODE_SLOTS; i++)
        s->knob_modes[i] = KM_INSTRUMENT + i;
    s->knob_target = KM_INSTRUMENT;
    s->pedal_cc[0] = 64;         /* sustain */
    s->pedal_cc[1] = 66;         /* sostenuto */
    s->strip_mod_cc = 1;
    s->daw_mode = DAW_AUTO;
    s->tempo_light = TEMPO_LIGHT_AUTO;
    s->contrast = 0xE0;          /* NI's value */
    s->led_dim = 0x1E;
    s->screensaver = 3;          /* 10 min */
}

static void clamp_u8(uint8_t *v, int lo, int hi, int def)
{
    if (*v < lo || *v > hi)
        *v = def;
}

static void clamp_s8(int8_t *v, int lo, int hi)
{
    if (*v < lo || *v > hi)
        *v = 0;
}

void settings_sanitize(settings_t *s)
{
    sp_config_t *c = &s->sp;
    clamp_u8(&c->scale_mode, 0, SCALE_MODE_COUNT - 1, SCALE_OFF);
    clamp_u8(&c->scale, 0, sp_scale_count - 1, 0);
    clamp_u8(&c->root, 0, 11, 0);
    clamp_u8(&c->chord, 0, CHORD_COUNT - 1, CHORD_OFF);
    clamp_u8(&c->arp_on, 0, 1, 0);
    clamp_u8(&c->arp_mode, 0, ARP_MODE_COUNT - 1, ARP_UP);
    clamp_u8(&c->arp_rate, 0, RATE_COUNT - 1, RATE_1_16);
    clamp_u8(&c->arp_octaves, 1, 4, 1);
    clamp_u8(&c->arp_gate, 10, 100, 50);
    clamp_u8(&c->arp_hold, 0, 1, 0);
    clamp_u8(&c->arp_swing, 50, 75, 50);
    if (c->bpm_x10 < 400 || c->bpm_x10 > 2400)
        c->bpm_x10 = 1200;
    clamp_u8(&c->ext_clock, 0, 1, 0);
    clamp_u8(&c->repeat_on, 0, 1, 0);
    clamp_u8(&c->repeat_vel, 0, REPEAT_VEL_COUNT - 1, REPEAT_VEL_KEY);
    clamp_u8(&c->strum_ms, 0, 200, 0);
    clamp_u8(&c->strum_dir, 0, STRUM_DIR_COUNT - 1, STRUM_UP);
    if (c->chord_mem_n > SP_CHORD_MAX)
        c->chord_mem_n = 0;
    for (int i = 0; i < SP_CHORD_MAX; i++)
        if (c->chord_mem[i] < 0 || c->chord_mem[i] > 48)
            c->chord_mem_n = 0;

    clamp_u8(&s->channel, 0, 15, 0);
    clamp_s8(&s->octave, -3, 3);
    clamp_s8(&s->transpose, -12, 12);
    clamp_u8(&s->vel_curve, 0, VEL_CURVE_COUNT - 1, VEL_LINEAR);
    clamp_u8(&s->vel_curve_prev, 0, VEL_CURVE_COUNT - 2, VEL_LINEAR);
    clamp_u8(&s->fixed_velocity, 1, 127, 100);
    clamp_u8(&s->split_on, 0, 1, 0);
    clamp_u8(&s->split_note, 0, 127, 60);
    clamp_u8(&s->lower_channel, 0, 15, 1);
    clamp_s8(&s->lower_octave, -3, 3);
    for (int p = 0; p < KNOB_PAGES; p++)
        for (int i = 0; i < BOARD_KNOBS; i++)
            clamp_u8(&s->knob_cc[p][i], 0, 127, 14 + BOARD_KNOBS * p + i);
    clamp_u8(&s->knob_page, 0, KNOB_PAGES - 1, 0);
    clamp_u8(&s->knob_mode, 0, KNOB_MODE_COUNT - 1, KNOBS_DAW);
    int any = 0;
    for (int i = 0; i < KNOB_MODE_SLOTS; i++) {
        clamp_u8(&s->knob_modes[i], 0, KM_COUNT - 1, KM_OFF);
        any |= s->knob_modes[i] != KM_OFF;
    }
    if (!any)
        s->knob_modes[0] = KM_INSTRUMENT;
    clamp_u8(&s->knob_target, KM_INSTRUMENT, KM_COUNT - 1, KM_INSTRUMENT);
    clamp_u8(&s->pedal_cc[0], 0, 127, 64);
    clamp_u8(&s->pedal_cc[1], 0, 127, 66);
    clamp_u8(&s->strip_mod_cc, 0, 127, 1);
    clamp_u8(&s->daw_mode, 0, DAW_MODE_COUNT - 1, DAW_AUTO);
    clamp_u8(&s->tempo_light, 0, TEMPO_LIGHT_COUNT - 1, TEMPO_LIGHT_AUTO);
    clamp_u8(&s->clock_out, 0, 1, 0);
    clamp_u8(&s->screensaver, 0, settings_screensaver_count - 1, 3);
}
