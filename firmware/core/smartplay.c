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
_Static_assert(sizeof sp_scales / sizeof sp_scales[0] == SP_SCALE_COUNT, "update SP_SCALE_COUNT");

const char *const sp_chord_names[CHORD_COUNT] = {"Off", "Octave", "Fifth", "Triad", "7th", "9th", "Learned"};
const char *const sp_arp_mode_names[ARP_MODE_COUNT] = {"Up", "Down", "Up/Down", "Order", "Random", "Chord"};
const char *const sp_arp_rate_names[RATE_COUNT] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

/* MIDI clock ticks (24 PPQN) per step */
static const uint8_t rate_ticks[RATE_COUNT] = {24, 12, 8, 6, 4, 3};

#define MAX_KEYS 16                       /* keys held (or latched) at once */
#define MAX_ARP  64                       /* pattern length */
#define CLOCK_LOST_US 500000u             /* external clock considered gone */

static sp_config_t *cfg;
static sp_out_fn out;

/* Each key remembers exactly the notes it produced, so its note-off always
   releases what its note-on started. */
typedef struct {
    uint8_t id, velocity, n, held;
    uint8_t sent;                         /* notes already sent (strum in progress) */
    uint32_t strum_us;                    /* time since the key went down */
    uint8_t notes[SP_CHORD_MAX];          /* in strum order */
} held_t;

static held_t keys[MAX_KEYS];
static uint8_t nkeys;
static uint8_t order_counter[MAX_KEYS], next_order;
static uint8_t strum_flip;

/* engine (arp / repeat) state */
enum { ENG_OFF, ENG_ARP, ENG_REPEAT };
static uint8_t pattern[MAX_ARP], pattern_vel[MAX_ARP], plen;
static int pos, dir = 1;
static uint8_t sounding[MAX_ARP], sounding_n;
static uint8_t engine_was, hold_was, ext_was;
static uint32_t tick_acc_us, step_ticks_done, gate_ticks;
static uint32_t rng = 0x12345678;
static uint16_t step_count;
static uint8_t beat_tick, expression = 100;

/* external clock: 0 = no Start/Stop seen yet, 1 = started, 2 = stopped */
static uint8_t ext_state;
static uint32_t since_clock_us = CLOCK_LOST_US;

static int engine(void)
{
    if (cfg->repeat_on) return ENG_REPEAT;
    if (cfg->arp_on) return ENG_ARP;
    return ENG_OFF;
}

static int clamp_note(int n)
{
    while (n > 127) n -= 12;
    while (n < 0) n += 12;
    return n;
}

/* a scale set from outside (the DAW's key), replacing cfg's root and type */
static sp_scale_t ext_scale;
static int8_t ext_root = -1;

static const sp_scale_t *scale(void)
{
    if (ext_root >= 0)
        return &ext_scale;
    return &sp_scales[cfg->scale < sp_scale_count ? cfg->scale : 0];
}

static int root(void) { return ext_root >= 0 ? ext_root : cfg->root; }

int sp_set_external_scale(int r, uint16_t mask, const char *name)
{
    if (r < 0) {
        ext_root = -1;
        return -1;
    }
    mask |= 1;
    int found = -1;
    for (int i = 0; i < sp_scale_count && found < 0; i++) {
        uint16_t m = 0;
        for (int k = 0; k < sp_scales[i].len; k++)
            m |= 1 << sp_scales[i].steps[k];
        if (m == (mask & 0xFFF))
            found = i;
    }
    if (found >= 0) {
        ext_scale = sp_scales[found];
    } else {
        ext_scale.name = name && name[0] ? name : "Custom";
        ext_scale.len = 0;
        for (int k = 0; k < 12; k++)
            if (mask & (1 << k))
                ext_scale.steps[ext_scale.len++] = k;
    }
    ext_root = r % 12;
    return found;
}

const char *sp_scale_name(void) { return scale()->name; }
int sp_scale_root(void) { return root(); }

uint16_t sp_scale_mask(void)
{
    const sp_scale_t *s = scale();
    uint16_t m = 0;
    for (int k = 0; k < s->len; k++)
        m |= (uint16_t)(1u << s->steps[k]);
    return m;
}

/* Snap to the nearest scale note; ties go down. */
static int quantize(int note)
{
    const sp_scale_t *s = scale();
    for (int d = 0; d < 12; d++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int n = note + sgn * d;
            int pc = ((n - root()) % 12 + 12) % 12;
            for (int i = 0; i < s->len; i++)
                if (s->steps[i] == pc)
                    return n;
        }
    }
    return note;
}

/* Easy mode: white keys play successive scale degrees starting at the root
   on C; black keys are silent (-1). */
static int easy_map(int note)
{
    static const int8_t white_index[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int pc = note % 12, oct = note / 12;
    if (white_index[pc] < 0)
        return -1;
    /* each keyboard octave stays in its own octave; scales shorter than 7
       notes wrap the extra white keys into the next octave */
    int idx = white_index[pc];
    const sp_scale_t *s = scale();
    return oct * 12 + root() + (idx / s->len) * 12 + s->steps[idx % s->len];
}

/* Position of note within the current scale as a degree index, or -1. */
static int degree_of(int note, int *octave)
{
    const sp_scale_t *s = scale();
    int rel = note - root();
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
    return root() + octave * 12 + s->steps[degree];
}

/* Expand one played note into the configured chord, lowest note first. */
static uint8_t build_chord(int root, uint8_t *dst)
{
    static const uint8_t degrees[CHORD_MEMORY][5] = {
        {0}, {0, 7}, {0, 4}, {0, 2, 4}, {0, 2, 4, 6}, {0, 2, 4, 6, 8},
    };
    static const uint8_t semis[CHORD_MEMORY][5] = {
        {0}, {0, 12}, {0, 7}, {0, 4, 7}, {0, 4, 7, 10}, {0, 4, 7, 10, 14},
    };
    static const uint8_t count[CHORD_MEMORY] = {1, 2, 2, 3, 4, 5};
    uint8_t c = cfg->chord < CHORD_COUNT ? cfg->chord : 0;

    if (c == CHORD_MEMORY) {
        if (!cfg->chord_mem_n || cfg->chord_mem_n > SP_CHORD_MAX) {
            dst[0] = clamp_note(root);
            return 1;
        }
        for (int i = 0; i < cfg->chord_mem_n; i++)
            dst[i] = clamp_note(root + cfg->chord_mem[i]);
        return cfg->chord_mem_n;
    }
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

/* ---------- direct play (engine off): strum ---------- */

static uint32_t strum_offset_us(const held_t *h, int i)
{
    if (!cfg->strum_ms || h->n < 2)
        return 0;
    return (uint32_t)i * cfg->strum_ms * 1000u / (h->n - 1);
}

static void strum_advance(held_t *h)
{
    while (h->sent < h->n && h->strum_us >= strum_offset_us(h, h->sent)) {
        out(h->notes[h->sent], h->velocity, 1);
        h->sent++;
    }
}

static void release_notes(held_t *h)
{
    for (int v = 0; v < h->sent; v++)
        out(h->notes[v], 0, 0);
    h->sent = 0;
}

/* ---------- engine ---------- */

static void engine_note_off(void)
{
    for (int i = 0; i < sounding_n; i++)
        out(sounding[i], 0, 0);
    sounding_n = 0;
}

static void engine_play(int idx)
{
    for (int i = 0; i < sounding_n; i++)
        if (sounding[i] == pattern[idx])
            return;                       /* already sounding in this step */
    uint8_t v = pattern_vel[idx];
    if (engine() == ENG_REPEAT && cfg->repeat_vel == REPEAT_VEL_STRIP)
        v = expression ? expression : 1;
    sounding[sounding_n++] = pattern[idx];
    out(pattern[idx], v, 1);
}

static void pattern_rebuild(void)
{
    uint8_t notes[MAX_KEYS * SP_CHORD_MAX], vels[MAX_KEYS * SP_CHORD_MAX], ord[MAX_KEYS * SP_CHORD_MAX];
    int n = 0, eng = engine();
    for (int k = 0; k < nkeys; k++)
        for (int v = 0; v < keys[k].n; v++) {
            notes[n] = keys[k].notes[v];
            vels[n] = keys[k].velocity;
            ord[n] = order_counter[k];
            n++;
        }
    /* sort: by key order for ORDER, by pitch otherwise */
    int by_order = eng == ENG_ARP && cfg->arp_mode == ARP_ORDER;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            int a = by_order ? ord[j - 1] : notes[j - 1];
            int b = by_order ? ord[j] : notes[j];
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
    int octs = eng == ENG_ARP && cfg->arp_octaves ? cfg->arp_octaves : 1;
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

static void engine_step(void)
{
    engine_note_off();
    if (!plen)
        return;
    if (engine() == ENG_REPEAT || cfg->arp_mode == ARP_CHORD) {
        for (int i = 0; i < plen; i++)
            engine_play(i);
        return;
    }
    int idx;
    switch (cfg->arp_mode) {
    case ARP_DOWN:
        idx = plen - 1 - (pos % plen);
        pos++;
        break;
    case ARP_UPDOWN:
        if (pos >= plen) pos = plen - 1;
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
    engine_play(idx);
}

static uint32_t step_len(uint16_t count)
{
    uint32_t len = rate_ticks[cfg->arp_rate < RATE_COUNT ? cfg->arp_rate : RATE_1_16] * 100u;
    uint32_t swing = cfg->arp_swing > 50 ? (cfg->arp_swing < 75 ? cfg->arp_swing : 75) : 50;
    /* pairs of steps: the first lasts swing%, the second (100 - swing)% of two steps */
    return (count & 1) ? len * 2 * (100 - swing) / 100 : len * 2 * swing / 100;
}

static uint32_t gate_of(uint32_t len)
{
    uint32_t g = cfg->arp_gate ? cfg->arp_gate : 50;
    return len * (g > 100 ? 100 : g) / 100;
}

static int running(void)
{
    if (!cfg->ext_clock)
        return 1;
    return ext_state == 1 || (ext_state == 0 && since_clock_us < CLOCK_LOST_US);
}

/* One 24-PPQN clock tick, from MIDI or the internal clock. Step lengths are
   counted in 1/100 ticks so gate and swing percentages stay exact. */
static void clock_tick(void)
{
    beat_tick = (beat_tick + 1) % 24;
    if (engine() == ENG_OFF || !running())
        return;
    step_ticks_done += 100;
    if (sounding_n && step_ticks_done >= gate_ticks)
        engine_note_off();
    uint32_t this_len = step_len(step_count);
    if (step_ticks_done >= this_len) {
        step_ticks_done -= this_len;
        step_count++;
        gate_ticks = gate_of(step_len(step_count));
        engine_step();
    }
}

static void engine_restart(void)
{
    pos = 0;
    dir = 1;
    step_count = 0;
    step_ticks_done = 0;
    gate_ticks = gate_of(step_len(0));
}

/* ---------- API ---------- */

void sp_defaults(sp_config_t *c)
{
    *c = (sp_config_t){
        .scale_mode = SCALE_OFF, .scale = 0, .root = 0, .chord = CHORD_OFF,
        .arp_on = 0, .arp_mode = ARP_UP, .arp_rate = RATE_1_16, .arp_octaves = 1,
        .arp_gate = 50, .arp_hold = 0, .arp_swing = 50, .bpm_x10 = 1200, .ext_clock = 0,
        .repeat_on = 0, .repeat_vel = REPEAT_VEL_KEY, .strum_ms = 0, .strum_dir = STRUM_UP,
        .chord_mem_n = 3, .chord_mem = {0, 4, 7},
    };
}

void sp_init(sp_config_t *c, sp_out_fn o)
{
    cfg = c;
    out = o;
    nkeys = 0;
    plen = 0;
    sounding_n = 0;
    engine_was = engine();
    hold_was = c->arp_hold;
    ext_was = c->ext_clock;
}

static void remove_key(int k)
{
    keys[k] = keys[--nkeys];
    order_counter[k] = order_counter[nkeys];
}

void sp_key(uint8_t id, uint8_t note, uint8_t velocity, int on)
{
    int eng = engine();
    if (on) {
        for (int k = 0; k < nkeys; k++)
            if (keys[k].id == id && keys[k].held)
                return;                   /* already down: ignore a duplicate */
        if (eng == ENG_ARP && cfg->arp_hold) {
            /* a new gesture after a full release replaces the latched notes */
            int any_down = 0;
            for (int k = 0; k < nkeys; k++)
                any_down |= keys[k].held;
            if (!any_down)
                nkeys = 0;
        }
        if (nkeys == MAX_KEYS)
            return;
        int n = note;
        if (cfg->scale_mode == SCALE_QUANTIZE)
            n = quantize(n);
        else if (cfg->scale_mode == SCALE_EASY && (n = easy_map(n)) < 0)
            return;
        held_t *h = &keys[nkeys];
        h->id = id;
        h->velocity = velocity ? velocity : 1;
        h->held = 1;
        h->sent = 0;
        h->strum_us = 0;
        h->n = build_chord(clamp_note(n), h->notes);
        if (cfg->strum_ms && eng == ENG_OFF) {
            int down = cfg->strum_dir == STRUM_DOWN || (cfg->strum_dir == STRUM_ALTERNATE && (strum_flip ^= 1));
            for (int i = 0; down && i < h->n / 2; i++) {
                uint8_t t = h->notes[i];
                h->notes[i] = h->notes[h->n - 1 - i];
                h->notes[h->n - 1 - i] = t;
            }
        }
        order_counter[nkeys] = next_order++;
        nkeys++;
        if (eng != ENG_OFF) {
            int first = plen == 0;
            pattern_rebuild();
            if (first && running()) {
                engine_restart();
                engine_step();
            }
        } else {
            strum_advance(h);
        }
        return;
    }
    for (int k = 0; k < nkeys; k++) {
        if (keys[k].id != id || !keys[k].held)
            continue;
        if (eng == ENG_ARP && cfg->arp_hold) {
            keys[k].held = 0;
        } else {
            if (eng == ENG_OFF)
                release_notes(&keys[k]);
            remove_key(k);
            if (eng != ENG_OFF) {
                pattern_rebuild();
                if (!plen)
                    engine_note_off();
            }
        }
        return;
    }
}

int sp_tick_us(uint32_t us)
{
    for (int k = 0; k < nkeys; k++)
        if (keys[k].sent < keys[k].n && engine() == ENG_OFF) {
            keys[k].strum_us += us;
            strum_advance(&keys[k]);
        }
    if (since_clock_us < CLOCK_LOST_US) {
        since_clock_us += us;
        if (since_clock_us >= CLOCK_LOST_US && cfg->ext_clock)
            engine_note_off();            /* clock vanished mid-gate */
    }
    if (cfg->ext_clock)
        return 0;
    uint32_t tick_us = 600000000u / ((cfg->bpm_x10 ? cfg->bpm_x10 : 1200) * 24u);
    int ticks = 0;
    tick_acc_us += us;
    while (tick_acc_us >= tick_us) {
        tick_acc_us -= tick_us;
        clock_tick();
        ticks++;
    }
    return ticks;
}

void sp_midi_clock(void)
{
    since_clock_us = 0;
    if (cfg->ext_clock)
        clock_tick();
}

void sp_midi_start(void)
{
    ext_state = 1;
    beat_tick = 23;                       /* the next clock is beat 1, tick 0 */
    engine_note_off();
    engine_restart();
    step_ticks_done = step_len(0) - 100;  /* first step fires on the first clock */
}

void sp_midi_continue(void)
{
    ext_state = 1;
}

void sp_midi_stop(void)
{
    ext_state = 2;
    if (cfg->ext_clock)
        engine_note_off();
}

void sp_panic(void)
{
    engine_note_off();
    for (int k = 0; k < nkeys; k++)
        release_notes(&keys[k]);
    nkeys = 0;
    plen = 0;
}

/* Turning the arp or repeat on or off changes who owns the sounding notes,
   so release everything; other changes only rebuild the pattern. */
void sp_config_changed(void)
{
    int eng = engine();
    if (cfg->ext_clock != ext_was) {
        ext_was = cfg->ext_clock;
        ext_state = 0;
        engine_note_off();
    }
    if (eng != engine_was) {
        sp_panic();
        engine_was = eng;
    } else if (eng != ENG_OFF) {
        if (hold_was && !cfg->arp_hold)
            for (int k = nkeys - 1; k >= 0; k--)
                if (!keys[k].held)
                    remove_key(k);         /* hold released: drop latched keys */
        pattern_rebuild();
        if (!plen)
            engine_note_off();
    }
    hold_was = cfg->arp_hold;
}

void sp_set_expression(uint8_t v) { expression = v; }
int sp_engine_on(void) { return engine() != ENG_OFF; }
int sp_engine_playing(void) { return engine() != ENG_OFF && plen && running(); }
int sp_clock_alive(void) { return cfg->ext_clock ? running() && since_clock_us < CLOCK_LOST_US : 1; }
uint8_t sp_beat_tick(void) { return beat_tick; }

int sp_held_count(void)
{
    int n = 0;
    for (int k = 0; k < nkeys; k++)
        n += keys[k].held;
    return n;
}

int sp_learn_chord(void)
{
    /* the played (pre-chord) note of each held key is its first generated
       note in up order; use the lowest note of each key */
    uint8_t notes[MAX_KEYS];
    int n = 0;
    for (int k = 0; k < nkeys; k++) {
        if (!keys[k].held)
            continue;
        uint8_t lo = 127;
        for (int v = 0; v < keys[k].n; v++)
            if (keys[k].notes[v] < lo)
                lo = keys[k].notes[v];
        notes[n++] = lo;
    }
    if (n < 2)
        return -1;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && notes[j - 1] > notes[j]; j--) {
            uint8_t t = notes[j]; notes[j] = notes[j - 1]; notes[j - 1] = t;
        }
    int m = n < SP_CHORD_MAX ? n : SP_CHORD_MAX;
    for (int i = 0; i < m; i++)
        cfg->chord_mem[i] = notes[i] - notes[0];
    cfg->chord_mem_n = m;
    return 0;
}
