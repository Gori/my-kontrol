#include "smartplay.h"

const sp_scale_t sp_scales[] = {
    {"Major",          7, {0, 2, 4, 5, 7, 9, 11}},
    {"Minor",          7, {0, 2, 3, 5, 7, 8, 10}},
    {"Harmonic Minor", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"Melodic Minor",  7, {0, 2, 3, 5, 7, 9, 11}},
    {"Dorian",         7, {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",       7, {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",         7, {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian",     7, {0, 2, 4, 5, 7, 9, 10}},
    {"Locrian",        7, {0, 1, 3, 5, 6, 8, 10}},
    {"Major Penta",    5, {0, 2, 4, 7, 9}},
    {"Minor Penta",    5, {0, 3, 5, 7, 10}},
    {"Blues",          6, {0, 3, 5, 6, 7, 10}},
    {"Whole Tone",     6, {0, 2, 4, 6, 8, 10}},
    {"Diminished",     8, {0, 2, 3, 5, 6, 8, 9, 11}},
    {"Phrygian Dom",   7, {0, 1, 4, 5, 7, 8, 10}},
    {"Hungarian Min",  7, {0, 2, 3, 6, 7, 8, 11}},
    {"Japanese",       5, {0, 1, 5, 7, 8}},
    {"Arabic",         7, {0, 1, 4, 5, 7, 8, 11}},
};
const uint8_t sp_scale_count = sizeof sp_scales / sizeof sp_scales[0];

const char *const sp_chord_names[CHORD_COUNT] = {"Off", "Octave", "Fifth", "Triad", "7th", "9th"};
const char *const sp_arp_mode_names[ARP_MODE_COUNT] = {"Up", "Down", "Up/Down", "Order", "Random", "Chord"};
const char *const sp_arp_rate_names[RATE_COUNT] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

/* MIDI clock ticks (24 PPQN) per arp step */
static const uint8_t rate_ticks[RATE_COUNT] = {24, 12, 8, 6, 4, 3};

#define MAX_KEYS   16  /* keys held at once */
#define MAX_VOICES 5   /* notes generated per key (root + up to 4 chord notes) */
#define MAX_ARP    64  /* arp pattern length */

static sp_config_t *cfg;
static sp_out_fn out;

/* Each held key remembers exactly the notes it produced, so note-off always
   releases what note-on started even if the config changed in between. */
typedef struct {
    uint8_t key, velocity, n, held;
    uint8_t notes[MAX_VOICES];
} held_t;

static held_t keys[MAX_KEYS];
static uint8_t nkeys;
static uint8_t order_counter[MAX_KEYS], next_order;

/* arp state */
static uint8_t pattern[MAX_ARP], pattern_vel[MAX_ARP], plen;
static int pos, dir = 1;
static uint8_t sounding[MAX_ARP], sounding_n;
static uint8_t arp_was_on;
static uint32_t tick_acc_us, step_ticks_done, gate_ticks;
static uint8_t clock_running = 1;
static uint32_t rng = 0x12345678;
static uint16_t step_count;

static int clamp_note(int n)
{
    while (n > 127) n -= 12;
    while (n < 0) n += 12;
    return n;
}

static const sp_scale_t *scale(void)
{
    return &sp_scales[cfg->scale < sp_scale_count ? cfg->scale : 0];
}

/* Snap to nearest scale note; ties go down. */
static int quantize(int note)
{
    const sp_scale_t *s = scale();
    for (int d = 0; d < 12; d++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int n = note + sgn * d;
            int pc = ((n - cfg->root) % 12 + 12) % 12;
            for (int i = 0; i < s->len; i++)
                if (s->steps[i] == pc)
                    return n;
        }
    }
    return note;
}

/* Easy mode: white keys play successive scale degrees starting at the root on C;
   black keys are ignored (return -1). */
static int easy_map(int note)
{
    static const int8_t white_index[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int pc = note % 12, oct = note / 12;
    if (white_index[pc] < 0)
        return -1;
    /* keep each keyboard octave in its own octave; scales shorter than 7
       notes wrap the extra white keys into the next octave */
    int idx = white_index[pc];
    const sp_scale_t *s = scale();
    return oct * 12 + cfg->root + (idx / s->len) * 12 + s->steps[idx % s->len];
}

/* Position of note within the current scale as a degree index, or -1. */
static int degree_of(int note, int *octave)
{
    const sp_scale_t *s = scale();
    int rel = note - cfg->root;
    int oct = rel >= 0 ? rel / 12 : -((11 - rel) / 12);
    int pc = rel - oct * 12;
    for (int i = 0; i < s->len; i++)
        if (s->steps[i] == pc) {
            *octave = oct;
            return i;
        }
    return -1;
}

static int scale_degree_note(int degree, int octave)
{
    const sp_scale_t *s = scale();
    octave += degree / s->len;
    degree %= s->len;
    return cfg->root + octave * 12 + s->steps[degree];
}

/* Expand one played note into the configured chord. */
static uint8_t build_chord(int root, uint8_t *dst)
{
    static const uint8_t degrees[CHORD_COUNT][MAX_VOICES] = {
        {0}, {0, 7}, {0, 4}, {0, 2, 4}, {0, 2, 4, 6}, {0, 2, 4, 6, 8},
    };
    static const uint8_t semis[CHORD_COUNT][MAX_VOICES] = {
        {0}, {0, 12}, {0, 7}, {0, 4, 7}, {0, 4, 7, 10}, {0, 4, 7, 10, 14},
    };
    static const uint8_t count[CHORD_COUNT] = {1, 2, 2, 3, 4, 5};
    uint8_t c = cfg->chord < CHORD_COUNT ? cfg->chord : 0;
    int oct, deg = cfg->scale_mode != SCALE_OFF ? degree_of(root, &oct) : -1;

    for (int i = 0; i < count[c]; i++) {
        int n;
        if (c == CHORD_OCTAVE)
            n = root + 12 * i;
        else if (deg >= 0)
            n = scale_degree_note(deg + degrees[c][i], oct);
        else
            n = root + semis[c][i];
        dst[i] = clamp_note(n);
    }
    return count[c];
}

static void arp_note_off(void)
{
    for (int i = 0; i < sounding_n; i++)
        out(sounding[i], 0, 0);
    sounding_n = 0;
}

static void arp_play(int idx)
{
    sounding[sounding_n++] = pattern[idx];
    out(pattern[idx], pattern_vel[idx], 1);
}

static void arp_rebuild(void)
{
    uint8_t notes[MAX_KEYS * MAX_VOICES], vels[MAX_KEYS * MAX_VOICES], ord[MAX_KEYS * MAX_VOICES];
    int n = 0;
    for (int k = 0; k < nkeys; k++)
        for (int v = 0; v < keys[k].n; v++) {
            notes[n] = keys[k].notes[v];
            vels[n] = keys[k].velocity;
            ord[n] = order_counter[k];
            n++;
        }
    /* sort: by order counter for ORDER, by pitch otherwise */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            int a = cfg->arp_mode == ARP_ORDER ? ord[j - 1] : notes[j - 1];
            int b = cfg->arp_mode == ARP_ORDER ? ord[j] : notes[j];
            if (a <= b)
                break;
            uint8_t t;
            t = notes[j]; notes[j] = notes[j - 1]; notes[j - 1] = t;
            t = vels[j]; vels[j] = vels[j - 1]; vels[j - 1] = t;
            t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t;
        }
    /* drop repeated pitches (chords from neighbouring keys overlap) */
    int m = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < m; j++)
            dup |= notes[j] == notes[i];
        if (!dup) {
            notes[m] = notes[i];
            vels[m++] = vels[i];
        }
    }
    n = m;
    plen = 0;
    int octs = cfg->arp_octaves ? cfg->arp_octaves : 1;
    for (int o = 0; o < octs; o++)
        for (int i = 0; i < n && plen < MAX_ARP; i++) {
            pattern[plen] = clamp_note(notes[i] + 12 * o);
            pattern_vel[plen++] = vels[i];
        }
    if (cfg->arp_mode == ARP_UPDOWN && pos >= plen)
        pos = plen ? plen - 1 : 0;
    if (pos > 30000)
        pos %= plen ? plen : 1;
}

static void arp_step(void)
{
    arp_note_off();
    if (!plen)
        return;
    if (cfg->arp_mode == ARP_CHORD) {
        for (int i = 0; i < plen; i++)
            arp_play(i);
        return;
    }
    int idx;
    switch (cfg->arp_mode) {
    case ARP_DOWN:
        idx = plen - 1 - (pos % plen);
        pos++;
        break;
    case ARP_UPDOWN:
        idx = pos;
        if (plen > 1) {
            if (pos + dir >= plen || pos + dir < 0)
                dir = -dir;
            pos += dir;
        }
        break;
    case ARP_RANDOM:
        rng = rng * 1103515245u + 12345u;
        idx = (rng >> 16) % plen;
        break;
    default:  /* UP, ORDER: pos keeps counting so notes added mid-run continue the sweep */
        idx = pos % plen;
        pos++;
        break;
    }
    arp_play(idx);
}

static uint32_t step_ticks(void)
{
    uint32_t t = rate_ticks[cfg->arp_rate < RATE_COUNT ? cfg->arp_rate : RATE_1_16];
    /* swing delays every second step; expressed in 1/100 ticks via gate math */
    return t;
}

/* One 24-PPQN clock tick, from MIDI or the internal clock. Ticks are counted
   in 1/100 units so gate and swing percentages stay exact. */
static void clock_tick(void)
{
    if (!cfg->arp_on || !clock_running)
        return;
    uint32_t len = step_ticks() * 100;
    uint32_t swing = cfg->arp_swing > 50 ? cfg->arp_swing : 50;
    /* pairs of steps: first lasts swing%, second (100-swing)% of two steps */
    uint32_t this_len = (step_count & 1) ? len * 2 * (100 - swing) / 100 : len * 2 * swing / 100;

    step_ticks_done += 100;
    if (sounding_n && step_ticks_done >= gate_ticks)
        arp_note_off();
    if (step_ticks_done >= this_len) {
        step_ticks_done -= this_len;
        step_count++;
        uint32_t next_len = (step_count & 1) ? len * 2 * (100 - swing) / 100 : len * 2 * swing / 100;
        gate_ticks = next_len * (cfg->arp_gate ? cfg->arp_gate : 50) / 100;
        arp_step();
    }
}

void sp_defaults(sp_config_t *c)
{
    *c = (sp_config_t){
        .scale_mode = SCALE_OFF, .scale = 0, .root = 0, .chord = CHORD_OFF,
        .arp_on = 0, .arp_mode = ARP_UP, .arp_rate = RATE_1_16, .arp_octaves = 1,
        .arp_gate = 50, .arp_hold = 0, .arp_swing = 50, .bpm_x10 = 1200, .ext_clock = 0,
    };
}

void sp_init(sp_config_t *c, sp_out_fn o)
{
    cfg = c;
    out = o;
    nkeys = 0;
    plen = 0;
    sounding_n = 0;
    arp_was_on = c->arp_on;
}

static void release_key_slot(int k)
{
    if (!cfg->arp_on)
        for (int v = 0; v < keys[k].n; v++)
            out(keys[k].notes[v], 0, 0);
    keys[k] = keys[--nkeys];
    order_counter[k] = order_counter[nkeys];
}

void sp_key(uint8_t key, uint8_t velocity, int on)
{
    if (on) {
        if (cfg->arp_on && cfg->arp_hold) {
            /* new gesture after a full release replaces the latched notes */
            int any_down = 0;
            for (int k = 0; k < nkeys; k++)
                any_down |= keys[k].held;
            if (!any_down)
                nkeys = 0;
        }
        if (nkeys == MAX_KEYS)
            return;
        int n = key;
        if (cfg->scale_mode == SCALE_QUANTIZE)
            n = quantize(n);
        else if (cfg->scale_mode == SCALE_EASY && (n = easy_map(n)) < 0)
            return;
        held_t *h = &keys[nkeys];
        h->key = key;
        h->velocity = velocity;
        h->held = 1;
        h->n = build_chord(clamp_note(n), h->notes);
        order_counter[nkeys] = next_order++;
        nkeys++;
        if (cfg->arp_on) {
            int first = plen == 0;
            arp_rebuild();
            if (first) {
                pos = 0;
                dir = 1;
                step_count = 0;
                step_ticks_done = 0;
                gate_ticks = step_ticks() * 100 * (cfg->arp_gate ? cfg->arp_gate : 50) / 100;
                arp_step();
            }
        } else {
            for (int v = 0; v < h->n; v++)
                out(h->notes[v], velocity, 1);
        }
        return;
    }
    for (int k = 0; k < nkeys; k++) {
        if (keys[k].key != key || !keys[k].held)
            continue;
        if (cfg->arp_on && cfg->arp_hold) {
            keys[k].held = 0;
        } else {
            release_key_slot(k);
            if (cfg->arp_on) {
                arp_rebuild();
                if (!plen)
                    arp_note_off();
            }
        }
        return;
    }
}

void sp_tick_us(uint32_t us)
{
    if (cfg->ext_clock)
        return;
    uint32_t tick_us = 600000000u / ((cfg->bpm_x10 ? cfg->bpm_x10 : 1200) * 24u);
    tick_acc_us += us;
    while (tick_acc_us >= tick_us) {
        tick_acc_us -= tick_us;
        clock_tick();
    }
}

void sp_midi_clock(void)
{
    if (cfg->ext_clock)
        clock_tick();
}

void sp_midi_start(void)
{
    clock_running = 1;
    step_ticks_done = 0;
    step_count = 0;
    pos = 0;
}

void sp_midi_stop(void)
{
    if (cfg->ext_clock) {
        clock_running = 0;
        arp_note_off();
    }
}

void sp_panic(void)
{
    arp_note_off();
    for (int k = 0; k < nkeys; k++)
        for (int v = 0; v < keys[k].n; v++)
            out(keys[k].notes[v], 0, 0);
    nkeys = 0;
    plen = 0;
}

/* Turning the arp on or off changes who owns the sounding notes, so release
   everything; other changes only re-sort the pattern. */
void sp_config_changed(void)
{
    if (cfg->arp_on != arp_was_on) {
        sp_panic();
        arp_was_on = cfg->arp_on;
    } else if (cfg->arp_on) {
        arp_rebuild();
    }
}
