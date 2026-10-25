#include "play.h"
#include "hal.h"
#include "midi.h"
#include "settings.h"
#include "smartplay.h"

enum { ZONE_UPPER, ZONE_LOWER };

uint8_t play_last_note, play_last_velocity;
uint16_t play_note_gen;

/* channel each zone is sending on; changes only after its notes are off */
static uint8_t zone_ch[2];
static uint8_t lower_active;

/* voice counts: the same pitch can come from two keys (chords, quantize);
   it sounds until the last one releases */
static uint8_t voices[2][128];

/* per key: zone and (lower zone) the note it started */
static uint8_t key_zone[BOARD_KEYS], key_note[BOARD_KEYS], key_down[BOARD_KEYS];

static int32_t strip_out[2] = {8192, 0}, strip_target[2] = {8192, 0};
static uint8_t mod_sent = 0xFF;
static uint8_t pedal_down[2];

static void send_both(uint8_t status, uint8_t a, uint8_t b)
{
    midi_keys3(status | zone_ch[ZONE_UPPER], a, b);
    if (lower_active && zone_ch[ZONE_LOWER] != zone_ch[ZONE_UPPER])
        midi_keys3(status | zone_ch[ZONE_LOWER], a, b);
}

static void voice(int zone, uint8_t note, uint8_t vel, int on)
{
    uint8_t *v = &voices[zone][note & 0x7F], ch = zone_ch[zone];
    if (on) {
        if (*v)
            midi_keys3(0x80 | ch, note, 0);   /* retrigger */
        if (*v < 255)
            (*v)++;
        midi_keys3(0x90 | ch, note, vel);
        play_last_note = note;
        play_last_velocity = vel;
        play_note_gen++;
    } else if (*v && !--*v) {
        midi_keys3(0x80 | ch, note, 0);
    }
}

static void zone_all_off(int zone)
{
    for (int n = 0; n < 128; n++)
        if (voices[zone][n]) {
            voices[zone][n] = 1;
            voice(zone, n, 0, 0);
        }
}

static void sp_out(uint8_t note, uint8_t vel, int on)
{
    voice(ZONE_UPPER, note, vel, on);
}

void play_init(void)
{
    zone_ch[ZONE_UPPER] = settings.channel;
    zone_ch[ZONE_LOWER] = settings.lower_channel;
    lower_active = settings.split_on;
    for (int k = 0; k < BOARD_KEYS; k++)
        key_down[k] = 0;
    sp_init(&settings.sp, sp_out);
}

static uint8_t curve(uint8_t v)
{
    switch (settings.vel_curve) {
    case VEL_SOFT: v = 127 - (127 - v) * (127 - v) / 127; break;
    case VEL_HARD: v = v * v / 127; break;
    case VEL_FIXED: v = settings.fixed_velocity; break;
    default: break;
    }
    return v ? v : 1;
}

void play_key(uint8_t key, uint8_t velocity, int on)
{
    if (key >= BOARD_KEYS)
        return;
    if (on) {
        if (key_down[key])
            return;
        int n = board_info.base_note + key + settings.transpose;
        int lower = settings.split_on && n < settings.split_note + settings.transpose;
        n += 12 * (lower ? settings.lower_octave : settings.octave);
        if (n < 0 || n > 127)
            return;
        key_down[key] = 1;
        key_zone[key] = lower;
        uint8_t v = curve(velocity);
        if (lower) {
            key_note[key] = n;
            voice(ZONE_LOWER, n, v, 1);
        } else {
            sp_key(key, n, v, 1);
        }
        return;
    }
    if (!key_down[key])
        return;
    key_down[key] = 0;
    if (key_zone[key] == ZONE_LOWER)
        voice(ZONE_LOWER, key_note[key], 0, 0);
    else
        sp_key(key, 0, 0, 0);
}

int play_keys_down(void)
{
    int n = 0;
    for (int k = 0; k < BOARD_KEYS; k++)
        n += key_down[k];
    return n;
}

uint8_t play_channel(void)
{
    return zone_ch[ZONE_UPPER];
}

/* ---------- strips and pedals ---------- */

void play_strip(uint8_t strip, int16_t v)
{
    if (strip == STRIP_PITCH)
        strip_target[0] = v < 0 ? 8192 : v;          /* released: back to centre */
    else if (v >= 0)
        strip_target[1] = v;                         /* mod holds when released */
}

static void strip_step(int s)
{
    /* slew like NI's firmware: small moves directly, big jumps in steps */
    int32_t *o = &strip_out[s], diff = strip_target[s] - *o;
    if (!diff)
        return;
    if (diff > 2560) diff = 640;
    else if (diff < -2560) diff = -640;
    *o += diff;
    if (s == STRIP_PITCH) {
        send_both(0xE0, *o & 0x7F, *o >> 7);
    } else if ((*o >> 7) != mod_sent) {
        mod_sent = *o >> 7;
        if (settings.sp.repeat_on && settings.sp.repeat_vel == REPEAT_VEL_STRIP)
            sp_set_expression(mod_sent);
        else
            send_both(0xB0, settings.strip_mod_cc, mod_sent);
    }
}

void play_pedal(uint8_t sw, int down)
{
    if (sw > 1 || pedal_down[sw] == !!down)
        return;
    pedal_down[sw] = !!down;
    send_both(0xB0, settings.pedal_cc[sw], down ? 127 : 0);
}

void play_pedal_unplugged(void)
{
    play_pedal(0, 0);
    play_pedal(1, 0);
}

void play_tick_ms(uint32_t ms)
{
    static uint32_t acc;
    for (acc += ms; acc >= 5; acc -= 5) {
        strip_step(STRIP_PITCH);
        strip_step(STRIP_MOD);
    }
}

/* ---------- settings ---------- */

/* Leave a channel cleanly: notes, bend and pedals off before switching. */
static void leave_channel(int zone)
{
    uint8_t ch = zone_ch[zone];
    zone_all_off(zone);
    if (zone == ZONE_UPPER || lower_active) {
        if (strip_out[0] != 8192)
            midi_keys3(0xE0 | ch, 0x00, 0x40);
        for (int s = 0; s < 2; s++)
            if (pedal_down[s])
                midi_keys3(0xB0 | ch, settings.pedal_cc[s], 0);
    }
}

void play_settings_changed(void)
{
    if (settings.channel != zone_ch[ZONE_UPPER]) {
        sp_panic();
        for (int k = 0; k < BOARD_KEYS; k++)
            if (key_zone[k] == ZONE_UPPER)
                key_down[k] = 0;
        leave_channel(ZONE_UPPER);
        zone_ch[ZONE_UPPER] = settings.channel;
    }
    if (settings.lower_channel != zone_ch[ZONE_LOWER] || settings.split_on != lower_active) {
        for (int k = 0; k < BOARD_KEYS; k++)
            if (key_zone[k] == ZONE_LOWER)
                key_down[k] = 0;
        leave_channel(ZONE_LOWER);
        zone_ch[ZONE_LOWER] = settings.lower_channel;
        lower_active = settings.split_on;
    }
    sp_config_changed();
}

void play_panic(void)
{
    sp_panic();
    zone_all_off(ZONE_UPPER);
    zone_all_off(ZONE_LOWER);
    for (int k = 0; k < BOARD_KEYS; k++)
        key_down[k] = 0;
    send_both(0xB0, 123, 0);     /* all notes off */
    send_both(0xB0, 64, 0);      /* sustain off */
}
