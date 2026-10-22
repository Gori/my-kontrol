#include "ui.h"
#include "gfx.h"
#include "settings.h"
#include "smartplay.h"
#include "settings_store.h"

#define VERSION "0.3"

/* ---------- menu model ---------- */

enum kind { K_SUB, K_ENUM, K_INT, K_ACTION, K_KNOBCC };
enum vtype { T_U8, T_S8, T_U16 };

typedef struct menu menu_t;
typedef struct {
    const char *name;
    uint8_t kind, vtype;
    void *ptr;
    int16_t min, max, step;
    const char *const *labels;                 /* K_ENUM */
    void (*fmt)(int v, char *buf);             /* optional custom value text */
    const menu_t *sub;                         /* K_SUB */
    void (*action)(void);                      /* K_ACTION */
} item_t;

struct menu {
    const char *title;
    const item_t *items;
    uint8_t n;
};

static const char *const off_on[] = {"Off", "On"};
static const char *const scale_modes[] = {"Off", "Quantize", "Easy"};
static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const vel_curves[] = {"Linear", "Soft", "Hard", "Fixed"};
static const char *const clock_src[] = {"Internal", "External"};
static const char *const daw_modes[] = {"MIDI", "Mackie"};

static void put_int(char *b, int v, int sign)
{
    char t[8];
    int n = 0, neg = v < 0;
    unsigned u = neg ? -v : v;
    do t[n++] = '0' + u % 10; while (u /= 10);
    if (neg) *b++ = '-';
    else if (sign && v > 0) *b++ = '+';
    while (n) *b++ = t[--n];
    *b = 0;
}

static void fmt_scale(int v, char *b)
{
    const char *s = sp_scales[v].name;
    while ((*b++ = *s++))
        ;
}

static void fmt_bpm(int v, char *b)
{
    put_int(b, v / 10, 0);
    while (*b) b++;
    *b++ = '.';
    *b++ = '0' + v % 10;
    *b = 0;
}

static void fmt_plus1(int v, char *b) { put_int(b, v + 1, 0); }
static void fmt_signed(int v, char *b) { put_int(b, v, 1); }
static void fmt_pct(int v, char *b) { put_int(b, v, 0); while (*b) b++; *b++ = '%'; *b = 0; }

static void show_about(void);
static void reset_settings(void);
static void preset_load(void);
static void preset_save(void);
static uint8_t preset_slot;

#define S (&settings)
#define SP (&settings.sp)

static const item_t scale_items[] = {
    {"Mode",  K_ENUM, T_U8, &SP->scale_mode, 0, 2, 1, scale_modes},
    {"Root",  K_ENUM, T_U8, &SP->root, 0, 11, 1, note_names},
    {"Scale", K_INT,  T_U8, &SP->scale, 0, 17, 1, 0, fmt_scale},
    {"Chord", K_ENUM, T_U8, &SP->chord, 0, CHORD_COUNT - 1, 1, sp_chord_names},
};
static const item_t arp_items[] = {
    {"Arp",     K_ENUM, T_U8,  &SP->arp_on, 0, 1, 1, off_on},
    {"Mode",    K_ENUM, T_U8,  &SP->arp_mode, 0, ARP_MODE_COUNT - 1, 1, sp_arp_mode_names},
    {"Rate",    K_ENUM, T_U8,  &SP->arp_rate, 0, RATE_COUNT - 1, 1, sp_arp_rate_names},
    {"Octaves", K_INT,  T_U8,  &SP->arp_octaves, 1, 4, 1},
    {"Gate",    K_INT,  T_U8,  &SP->arp_gate, 10, 100, 5, 0, fmt_pct},
    {"Swing",   K_INT,  T_U8,  &SP->arp_swing, 50, 75, 1, 0, fmt_pct},
    {"Hold",    K_ENUM, T_U8,  &SP->arp_hold, 0, 1, 1, off_on},
    {"Tempo",   K_INT,  T_U16, &SP->bpm_x10, 400, 2400, 10, 0, fmt_bpm},
    {"Clock",   K_ENUM, T_U8,  &SP->ext_clock, 0, 1, 1, clock_src},
};
static const item_t keys_items[] = {
    {"Channel",   K_INT,  T_U8, &S->channel, 0, 15, 1, 0, fmt_plus1},
    {"Octave",    K_INT,  T_S8, &S->octave, -3, 3, 1, 0, fmt_signed},
    {"Transpose", K_INT,  T_S8, &S->transpose, -12, 12, 1, 0, fmt_signed},
    {"Velocity",  K_ENUM, T_U8, &S->vel_curve, 0, VEL_CURVE_COUNT - 1, 1, vel_curves},
    {"Fixed Vel", K_INT,  T_U8, &S->fixed_velocity, 1, 127, 1},
};
static const item_t knob_items[] = {
    {"Page", K_INT, T_U8, &S->knob_page, 0, 3, 1, 0, fmt_plus1},
    {"Knob 1 CC", K_KNOBCC, T_U8, 0, 0, 127, 0},
    {"Knob 2 CC", K_KNOBCC, T_U8, 0, 0, 127, 1},
    {"Knob 3 CC", K_KNOBCC, T_U8, 0, 0, 127, 2},
    {"Knob 4 CC", K_KNOBCC, T_U8, 0, 0, 127, 3},
    {"Knob 5 CC", K_KNOBCC, T_U8, 0, 0, 127, 4},
    {"Knob 6 CC", K_KNOBCC, T_U8, 0, 0, 127, 5},
    {"Knob 7 CC", K_KNOBCC, T_U8, 0, 0, 127, 6},
    {"Knob 8 CC", K_KNOBCC, T_U8, 0, 0, 127, 7},
    {"Mod Strip", K_INT, T_U8, &S->strip_mod_cc, 0, 127, 1},
    {"Pedal Tip", K_INT, T_U8, &S->pedal_cc[0], 0, 127, 1},
    {"Pedal Ring", K_INT, T_U8, &S->pedal_cc[1], 0, 127, 1},
};
static const item_t system_items[] = {
    {"DAW Mode", K_ENUM, T_U8, &S->daw_mode, 0, 1, 1, daw_modes},
    {"Contrast", K_INT,  T_U8, &S->contrast, 0, 255, 16},
    {"Preset",   K_INT,  T_U8, &preset_slot, 0, 7, 1, 0, fmt_plus1},
    {"Load Preset", K_ACTION, 0, 0, 0, 0, 0, 0, 0, 0, preset_load},
    {"Save Preset", K_ACTION, 0, 0, 0, 0, 0, 0, 0, 0, preset_save},
    {"Reset All", K_ACTION, 0, 0, 0, 0, 0, 0, 0, 0, reset_settings},
    {"About",    K_ACTION, 0, 0, 0, 0, 0, 0, 0, 0, show_about},
};

#define MENU(t, a) {t, a, sizeof a / sizeof a[0]}
static const menu_t scale_menu = MENU("Scale & Chord", scale_items);
static const menu_t arp_menu = MENU("Arpeggiator", arp_items);
static const menu_t keys_menu = MENU("Keys", keys_items);
static const menu_t knob_menu = MENU("Knobs", knob_items);
static const menu_t system_menu = MENU("System", system_items);
static const item_t main_items[] = {
    {"Scale & Chord", K_SUB, .sub = &scale_menu},
    {"Arpeggiator",   K_SUB, .sub = &arp_menu},
    {"Keys",          K_SUB, .sub = &keys_menu},
    {"Knobs",         K_SUB, .sub = &knob_menu},
    {"System",        K_SUB, .sub = &system_menu},
};
static const menu_t main_menu = MENU("Menu", main_items);

/* ---------- state ---------- */

#define DEPTH 4
static const menu_t *stack[DEPTH];
static uint8_t sel[DEPTH], top[DEPTH];
static int8_t depth = -1;   /* -1 = home screen */
static uint8_t editing;
static uint8_t dirty = 1;

static char note_title[22], note_value[22];
static uint32_t note_ms;    /* overlay remaining */
static char daw_top[22], daw_bottom[22];
static uint8_t about;

static int get(const item_t *it)
{
    if (it->kind == K_KNOBCC)                       /* step = knob index, current page */
        return settings.knob_cc[settings.knob_page][it->step];
    switch (it->vtype) {
    case T_S8: return *(int8_t *)it->ptr;
    case T_U16: return *(uint16_t *)it->ptr;
    default: return *(uint8_t *)it->ptr;
    }
}

static void set(const item_t *it, int v)
{
    if (v < it->min) v = it->min;
    if (v > it->max) v = it->max;
    if (it->kind == K_KNOBCC) {
        settings.knob_cc[settings.knob_page][it->step] = v;
        return;
    }
    switch (it->vtype) {
    case T_S8: *(int8_t *)it->ptr = v; break;
    case T_U16: *(uint16_t *)it->ptr = v; break;
    default: *(uint8_t *)it->ptr = v; break;
    }
}

static void value_text(const item_t *it, char *b)
{
    int v = get(it);
    if (it->fmt) it->fmt(v, b);
    else if (it->kind == K_ENUM) {
        const char *s = it->labels[v];
        while ((*b++ = *s++))
            ;
    } else put_int(b, v, 0);
}

static void copy(char *d, const char *s, int n)
{
    while (--n > 0 && *s)
        *d++ = *s++;
    *d = 0;
}

static void preset_load(void)
{
    char v[8] = "Slot 1";
    v[5] = '1' + preset_slot;
    if (presets_load(preset_slot) == 0) {
        app_settings_changed();
        ui_notify("Loaded", v);
    } else {
        ui_notify("Empty", v);
    }
}

static void preset_save(void)
{
    char v[8] = "Slot 1";
    v[5] = '1' + preset_slot;
    ui_notify(presets_save(preset_slot) == 0 ? "Saved" : "Save failed", v);
}

static void reset_settings(void)
{
    settings_defaults(&settings);
    app_settings_changed();
    ui_notify("Settings", "Reset");
}

static void show_about(void)
{
    about = 1;
    dirty = 1;
}

void ui_init(void)
{
    depth = -1;
    dirty = 1;
}

void ui_open_menu(int which)
{
    depth = 1;
    editing = 0;
    about = 0;
    stack[0] = &main_menu;
    sel[0] = which;
    top[0] = which > 2 ? which - 2 : 0;
    stack[1] = main_items[which].sub;
    sel[1] = top[1] = 0;
    dirty = 1;
}

int ui_dirty(void) { return dirty; }
int ui_menu_open(void) { return depth >= 0 || about; }

void ui_notify(const char *title, const char *value)
{
    copy(note_title, title, sizeof note_title);
    copy(note_value, value, sizeof note_value);
    note_ms = 1200;
    dirty = 1;
}

void ui_set_daw_text(const char *t, const char *b)
{
    copy(daw_top, t, sizeof daw_top);
    copy(daw_bottom, b, sizeof daw_bottom);
    if (depth < 0)
        dirty = 1;
}

void ui_tick_ms(uint32_t ms)
{
    if (note_ms) {
        note_ms = ms >= note_ms ? 0 : note_ms - ms;
        if (!note_ms)
            dirty = 1;
    }
}

static void move(int d)
{
    const menu_t *m = stack[depth];
    int s = sel[depth] + d;
    if (s < 0) s = 0;
    if (s >= m->n) s = m->n - 1;
    sel[depth] = s;
    if (s < top[depth]) top[depth] = s;
    if (s > top[depth] + 2) top[depth] = s - 2;
}

static void back(void)
{
    if (about) { about = 0; return; }
    if (editing) { editing = 0; return; }
    if (depth >= 0) depth--;
}

static void enter(void)
{
    if (about) { about = 0; return; }
    if (depth < 0) {
        depth = 0;
        stack[0] = &main_menu;
        sel[0] = top[0] = 0;
        return;
    }
    const item_t *it = &stack[depth]->items[sel[depth]];
    switch (it->kind) {
    case K_SUB:
        if (depth + 1 < DEPTH) {
            depth++;
            stack[depth] = it->sub;
            sel[depth] = top[depth] = 0;
        }
        break;
    case K_ACTION:
        it->action();
        break;
    default:
        editing = !editing;
        break;
    }
}

void ui_event(uint8_t type, int8_t index, int16_t value)
{
    (void)index;
    dirty = 1;
    switch (type) {
    case EV_SHOW_MENU:
        if (depth < 0) enter();
        break;
    case EV_ENC_TURN:
        if (depth < 0 || about) break;
        if (editing) {
            const item_t *it = &stack[depth]->items[sel[depth]];
            set(it, get(it) + value * (it->kind == K_KNOBCC || !it->step ? 1 : it->step));
            app_settings_changed();
        } else {
            move(value);
        }
        break;
    case EV_ENC_UP: if (depth >= 0 && !editing) move(-1); break;
    case EV_ENC_DOWN: if (depth >= 0 && !editing) move(1); break;
    case EV_ENC_PUSH:
    case EV_ENC_RIGHT:
        enter();
        break;
    case EV_ENC_LEFT:
    case EV_BACK:
        back();
        break;
    default:
        break;
    }
}

/* ---------- rendering ---------- */

static void render_home(void)
{
    char b[24], *p = b;
    const settings_t *s = &settings;

    /* status line: channel, octave, transpose */
    p = b;
    *p++ = 'C'; *p++ = 'H';
    put_int(p, s->channel + 1, 0); while (*p) p++;
    *p++ = ' '; *p++ = 'O'; *p++ = 'C'; *p++ = 'T';
    put_int(p, s->octave, 1); while (*p) p++;
    *p++ = ' '; *p++ = 'T'; *p++ = 'R';
    put_int(p, s->transpose, 1); while (*p) p++;
    *p = 0;
    gfx_text(0, 0, b, &font_small);
    if (s->daw_mode)
        gfx_text(128 - 4 * 5, 0, "MCU", &font_small), gfx_invert(127 - 4 * 5, 0, 21, 8);

    /* centre: DAW text if Mackie is feeding us, else the active play mode */
    if (s->daw_mode && daw_top[0]) {
        gfx_text(0, 10, daw_top, &font_small);
        gfx_text(0, 19, daw_bottom, &font_small);
    } else if (s->sp.scale_mode != SCALE_OFF) {
        p = b;
        const char *n = note_names[s->sp.root];
        while (*n) *p++ = *n++;
        *p++ = ' ';
        n = sp_scales[s->sp.scale].name;
        while (*n && p < b + 15) *p++ = *n++;
        *p = 0;
        gfx_text_center(9, b, &font_large);
    } else {
        gfx_text_center(9, "Ready", &font_large);
    }

    /* bottom line: arp/chord summary */
    p = b;
    if (s->sp.arp_on) {
        const char *n = "ARP ";
        while (*n) *p++ = *n++;
        n = sp_arp_rate_names[s->sp.arp_rate];
        while (*n) *p++ = *n++;
        *p++ = ' ';
        if (s->sp.ext_clock) { n = "EXT"; while (*n) *p++ = *n++; }
        else { fmt_bpm(s->sp.bpm_x10, p); while (*p) p++; }
    }
    if (s->sp.chord != CHORD_OFF) {
        if (p != b) *p++ = ' ';
        const char *n = sp_chord_names[s->sp.chord];
        while (*n) *p++ = *n++;
    }
    *p = 0;
    if (!s->daw_mode || !daw_top[0])
        gfx_text(0, 25, b, &font_small);
}

static void render_menu(void)
{
    const menu_t *m = stack[depth];
    /* title followed by a rule, so it never looks like the selected row */
    int tx = gfx_text(1, 0, m->title, &font_small);
    gfx_fill(tx + 3, 3, 125 - tx - 3, 1, 1);
    /* scroll position */
    if (m->n > 3) {
        int h = 24 * 3 / m->n, y = 8 + (24 - h) * top[depth] / (m->n - 3);
        gfx_fill(126, y, 2, h, 1);
    }
    for (int r = 0; r < 3 && top[depth] + r < m->n; r++) {
        int i = top[depth] + r, y = 8 + r * 8;
        const item_t *it = &m->items[i];
        gfx_text(2, y, it->name, &font_small);
        char v[22];
        if (it->kind == K_SUB) { v[0] = '>'; v[1] = 0; }
        else if (it->kind == K_ACTION) v[0] = 0;
        else value_text(it, v);
        int w = gfx_text_width(v, &font_small);
        gfx_text(123 - w, y, v, &font_small);
        if (i == sel[depth]) {
            if (editing)
                gfx_invert(122 - w, y, w + 2, 8);    /* value highlighted while editing */
            else
                gfx_invert(0, y, 125, 8);
        }
    }
}

static void render_about(void)
{
    gfx_text_center(0, "Oskar's Firmware", &font_small);
    gfx_text_center(9, "v" VERSION, &font_large);
    gfx_text_center(25, "Komplete Kontrol M32", &font_small);
}

static void render_overlay(void)
{
    gfx_fill(4, 2, 120, 28, 0);
    gfx_fill(4, 2, 120, 1, 1);
    gfx_fill(4, 29, 120, 1, 1);
    gfx_fill(4, 2, 1, 28, 1);
    gfx_fill(123, 2, 1, 28, 1);
    gfx_text_center(4, note_title, &font_small);
    gfx_text_center(12, note_value, &font_large);
}

void ui_render(void)
{
    if (!dirty)
        return;
    dirty = 0;
    gfx_clear();
    if (about) render_about();
    else if (depth < 0) render_home();
    else render_menu();
    if (note_ms)
        render_overlay();
    gfx_flush();
}
