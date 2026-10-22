#include "ui.h"
#include "app.h"
#include "gfx.h"
#include "hal.h"
#include "host.h"
#include "mcu.h"
#include "play.h"
#include "settings.h"
#include "settings_store.h"
#include "smartplay.h"
#include "version.h"

/* ---------- text helpers ---------- */

static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *put_str(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}

static char *put_int(char *b, int v, int sign)
{
    char t[8];
    int n = 0, neg = v < 0;
    unsigned u = neg ? -v : v;
    do t[n++] = '0' + u % 10; while (u /= 10);
    if (neg) *b++ = '-';
    else if (sign && v > 0) *b++ = '+';
    while (n) *b++ = t[--n];
    *b = 0;
    return b;
}

/* MIDI note name, 60 = C3 (as in Live, Logic and Komplete Kontrol) */
static char *put_note(char *b, int n)
{
    b = put_str(b, note_names[n % 12]);
    return put_int(b, n / 12 - 2, 0);
}

static char *put_bpm(char *b, int v)
{
    b = put_int(b, v / 10, 0);
    *b++ = '.';
    *b++ = '0' + v % 10;
    *b = 0;
    return b;
}

static void copy(char *d, const char *s, int n)
{
    while (--n > 0 && *s)
        *d++ = *s++;
    *d = 0;
}

/* ---------- menu model ---------- */

enum kind { K_SUB, K_ENUM, K_INT, K_ACTION, K_CONFIRM, K_KNOBCC };
enum vtype { T_U8, T_S8, T_U16 };

typedef struct menu menu_t;
typedef struct {
    const char *name;
    uint8_t kind, vtype;
    void *ptr;
    int16_t min, max, step;
    const char *const *labels;                 /* K_ENUM */
    void (*fmt)(int v, char *buf);             /* custom value text */
    const menu_t *sub;                         /* K_SUB */
    void (*action)(void);                      /* K_ACTION, K_CONFIRM */
    const char *confirm;                       /* K_CONFIRM question */
} item_t;

struct menu {
    const char *title;
    const item_t *items;
    uint8_t n;
};

static const char *const off_on[] = {"Off", "On"};
static const char *const scale_modes[] = {"Off", "Quantize", "Easy"};
static const char *const vel_curves[] = {"Soft", "Linear", "Hard", "Fixed"};
static const char *const clock_src[] = {"Internal", "External"};
static const char *const daw_modes[] = {"Auto", "Off"};
static const char *const knob_modes[] = {"DAW", "MIDI CC"};
static const char *const tempo_lights[] = {"Auto", "Always", "Off"};
static const char *const strum_dirs[] = {"Up", "Down", "Alternate"};
static const char *const repeat_vels[] = {"Key", "Mod Strip"};

static void fmt_scale(int v, char *b) { put_str(b, sp_scales[v].name); }
static void fmt_bpm(int v, char *b) { put_bpm(b, v); }
static void fmt_plus1(int v, char *b) { put_int(b, v + 1, 0); }
static void fmt_signed(int v, char *b) { put_int(b, v, 1); }
static void fmt_pct(int v, char *b) { put_str(put_int(b, v, 0), "%"); }
static void fmt_note(int v, char *b) { put_note(b, v); }
static void fmt_ms(int v, char *b) { if (v) put_str(put_int(b, v, 0), " ms"); else put_str(b, "Off"); }
static void fmt_saver(int v, char *b)
{
    int m = settings_screensaver_min[v];
    if (m) put_str(put_int(b, m, 0), " min");
    else put_str(b, "Off");
}
static void fmt_slot(int v, char *b)
{
    b = put_int(b, v + 1, 0);
    put_str(b, presets_used(v) ? " (used)" : " (empty)");
}

static void act_learn(void);
static void act_panic(void);
static void act_load(void);
static void act_save(void);
static void act_reset(void);
static void act_update(void);
static void act_about(void);
static uint8_t preset_slot;

#define S (&settings)
#define SP (&settings.sp)

static const item_t scale_items[] = {
    {"Scale",     K_ENUM, T_U8, &SP->scale_mode, 0, SCALE_MODE_COUNT - 1, 1, scale_modes},
    {"Root",      K_ENUM, T_U8, &SP->root, 0, 11, 1, note_names},
    {"Type",      K_INT,  T_U8, &SP->scale, 0, SP_SCALE_COUNT - 1, 1, 0, fmt_scale},
    {"Chord",     K_ENUM, T_U8, &SP->chord, 0, CHORD_COUNT - 1, 1, sp_chord_names},
    {"Learn Chord", K_ACTION, .action = act_learn},
    {"Strum",     K_INT,  T_U8, &SP->strum_ms, 0, 200, 10, 0, fmt_ms},
    {"Strum Dir", K_ENUM, T_U8, &SP->strum_dir, 0, STRUM_DIR_COUNT - 1, 1, strum_dirs},
};
static const item_t arp_items[] = {
    {"Arp",        K_ENUM, T_U8,  &SP->arp_on, 0, 1, 1, off_on},
    {"Mode",       K_ENUM, T_U8,  &SP->arp_mode, 0, ARP_MODE_COUNT - 1, 1, sp_arp_mode_names},
    {"Rate",       K_ENUM, T_U8,  &SP->arp_rate, 0, RATE_COUNT - 1, 1, sp_arp_rate_names},
    {"Octaves",    K_INT,  T_U8,  &SP->arp_octaves, 1, 4, 1},
    {"Gate",       K_INT,  T_U8,  &SP->arp_gate, 10, 100, 5, 0, fmt_pct},
    {"Swing",      K_INT,  T_U8,  &SP->arp_swing, 50, 75, 1, 0, fmt_pct},
    {"Hold",       K_ENUM, T_U8,  &SP->arp_hold, 0, 1, 1, off_on},
    {"Note Repeat", K_ENUM, T_U8, &SP->repeat_on, 0, 1, 1, off_on},
    {"Repeat Vel", K_ENUM, T_U8,  &SP->repeat_vel, 0, REPEAT_VEL_COUNT - 1, 1, repeat_vels},
    {"Tempo",      K_INT,  T_U16, &SP->bpm_x10, 400, 2400, 10, 0, fmt_bpm},
    {"Clock",      K_ENUM, T_U8,  &SP->ext_clock, 0, 1, 1, clock_src},
    {"Clock Out",  K_ENUM, T_U8,  &S->clock_out, 0, 1, 1, off_on},
};
static const item_t keys_items[] = {
    {"Channel",   K_INT,  T_U8, &S->channel, 0, 15, 1, 0, fmt_plus1},
    {"Octave",    K_INT,  T_S8, &S->octave, -3, 3, 1, 0, fmt_signed},
    {"Transpose", K_INT,  T_S8, &S->transpose, -12, 12, 1, 0, fmt_signed},
    {"Velocity",  K_ENUM, T_U8, &S->vel_curve, 0, VEL_CURVE_COUNT - 1, 1, vel_curves},
    {"Fixed Vel", K_INT,  T_U8, &S->fixed_velocity, 1, 127, 1},
    {"Split",     K_ENUM, T_U8, &S->split_on, 0, 1, 1, off_on},
    {"Split At",  K_INT,  T_U8, &S->split_note, 24, 108, 1, 0, fmt_note},
    {"Lower Ch",  K_INT,  T_U8, &S->lower_channel, 0, 15, 1, 0, fmt_plus1},
    {"Lower Oct", K_INT,  T_S8, &S->lower_octave, -3, 3, 1, 0, fmt_signed},
    {"All Notes Off", K_ACTION, .action = act_panic},
};
static const item_t control_items[] = {
    {"Knob Page", K_INT, T_U8, &S->knob_page, 0, KNOB_PAGES - 1, 1, 0, fmt_plus1},
    {"Knob 1 CC", K_KNOBCC, T_U8, 0, 0, 127, 0},
    {"Knob 2 CC", K_KNOBCC, T_U8, 0, 0, 127, 1},
    {"Knob 3 CC", K_KNOBCC, T_U8, 0, 0, 127, 2},
    {"Knob 4 CC", K_KNOBCC, T_U8, 0, 0, 127, 3},
    {"Knob 5 CC", K_KNOBCC, T_U8, 0, 0, 127, 4},
    {"Knob 6 CC", K_KNOBCC, T_U8, 0, 0, 127, 5},
    {"Knob 7 CC", K_KNOBCC, T_U8, 0, 0, 127, 6},
    {"Knob 8 CC", K_KNOBCC, T_U8, 0, 0, 127, 7},
    {"Knobs in DAW", K_ENUM, T_U8, &S->knob_mode, 0, KNOB_MODE_COUNT - 1, 1, knob_modes},
    {"Mod Strip CC", K_INT, T_U8, &S->strip_mod_cc, 0, 127, 1},
    {"Pedal Tip CC", K_INT, T_U8, &S->pedal_cc[0], 0, 127, 1},
    {"Pedal Ring CC", K_INT, T_U8, &S->pedal_cc[1], 0, 127, 1},
};
static const item_t preset_items[] = {
    {"Slot", K_INT, T_U8, &preset_slot, 0, 7, 1, 0, fmt_slot},
    {"Load", K_ACTION, .action = act_load},
    {"Save", K_ACTION, .action = act_save},
};
static const item_t system_items[] = {
    {"DAW Control",  K_ENUM, T_U8, &S->daw_mode, 0, DAW_MODE_COUNT - 1, 1, daw_modes},
    {"Tempo Light",  K_ENUM, T_U8, &S->tempo_light, 0, TEMPO_LIGHT_COUNT - 1, 1, tempo_lights},
    {"Light Level",  K_INT,  T_U8, &S->led_dim, 0, 120, 5},
    {"Contrast",     K_INT,  T_U8, &S->contrast, 0, 255, 16},
    {"Screen Saver", K_INT,  T_U8, &S->screensaver, 0, 4, 1, 0, fmt_saver},
    {"4D as Keys",   K_ENUM, T_U8, &S->nav_keys, 0, 1, 1, off_on},
    {"Update Mode",  K_CONFIRM, .action = act_update, .confirm = "Enter update mode?"},
    {"Reset All",    K_CONFIRM, .action = act_reset, .confirm = "Reset all settings?"},
    {"About",        K_ACTION, .action = act_about},
};

#define MENU(t, a) {t, a, sizeof a / sizeof a[0]}
static const menu_t scale_menu = MENU("Scale & Chord", scale_items);
static const menu_t arp_menu = MENU("Arp & Repeat", arp_items);
static const menu_t keys_menu = MENU("Keys", keys_items);
static const menu_t control_menu = MENU("Controls", control_items);
static const menu_t preset_menu = MENU("Presets", preset_items);
static const menu_t system_menu = MENU("System", system_items);
static const item_t main_items[] = {
    {"Scale & Chord", K_SUB, .sub = &scale_menu},
    {"Arp & Repeat",  K_SUB, .sub = &arp_menu},
    {"Keys",          K_SUB, .sub = &keys_menu},
    {"Controls",      K_SUB, .sub = &control_menu},
    {"Presets",       K_SUB, .sub = &preset_menu},
    {"System",        K_SUB, .sub = &system_menu},
};
static const menu_t main_menu = MENU("Menu", main_items);

/* ---------- state ---------- */

enum { DLG_NONE, DLG_CONFIRM, DLG_LEARN, DLG_ABOUT, DLG_UPDATE };
enum { HOME_PLAY, HOME_KNOBS, HOME_PAGES };
enum { POP_NONE, POP_TEXT, POP_KNOB };

#define DEPTH 3
#define ROWS 3
static const menu_t *stack[DEPTH];
static uint8_t sel[DEPTH], top[DEPTH];
static int8_t depth = -1;               /* -1 = home screen */
static uint8_t editing, direct;         /* direct: opened by a shortcut, Back goes home */
static uint8_t dialog;
static const item_t *confirm_item;
static uint8_t home_page;
static uint8_t dirty = 1;

static uint8_t pop;
static uint8_t pop_knob;
static char pop_title[22], pop_value[22];
static uint32_t pop_ms;
static uint32_t blink_ms;
static uint8_t blink;

static uint16_t seen_host, seen_lcd, seen_tc, seen_note;
static int8_t seen_link = -1;

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
    else if (it->kind == K_ENUM) put_str(b, it->labels[v]);
    else put_int(b, v, 0);
}

/* ---------- actions ---------- */

static void act_learn(void) { ui_learn_begin(); }
static void act_panic(void) { app_panic(); ui_notify("All Notes Off", "Done"); }
static void act_about(void) { dialog = DLG_ABOUT; }
static void act_update(void) { dialog = DLG_UPDATE; dirty = 1; ui_render(); app_enter_bootloader(); }

static void act_load(void)
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

static void do_save(void)
{
    char v[8] = "Slot 1";
    v[5] = '1' + preset_slot;
    ui_notify(presets_save(preset_slot) == 0 ? "Saved" : "Save failed", v);
}

static const item_t save_confirm = {"Save", K_CONFIRM, .action = do_save, .confirm = "Overwrite slot?"};

static void act_save(void)
{
    if (presets_used(preset_slot)) {
        confirm_item = &save_confirm;
        dialog = DLG_CONFIRM;
    } else {
        do_save();
    }
}

static void act_reset(void)
{
    settings_defaults(&settings);
    app_settings_changed();
    ui_notify("Settings", "Reset");
}

/* ---------- navigation ---------- */

void ui_init(void)
{
    depth = -1;
    dirty = 1;
}

void ui_invalidate(void) { dirty = 1; }
int ui_menu_open(void) { return depth >= 0 || dialog; }

static void go_home(void)
{
    depth = -1;
    editing = 0;
    dialog = DLG_NONE;
    direct = 0;
}

void ui_open_menu(int which)
{
    go_home();
    depth = 1;
    direct = 1;
    stack[0] = &main_menu;
    sel[0] = which;
    top[0] = which >= ROWS ? which - ROWS + 1 : 0;
    stack[1] = main_items[which].sub;
    sel[1] = top[1] = 0;
    dirty = 1;
}

void ui_toggle_menu(void)
{
    if (ui_menu_open()) {
        go_home();
    } else {
        depth = 0;
        stack[0] = &main_menu;
        sel[0] = top[0] = 0;
    }
    dirty = 1;
}

void ui_home_step(int d)
{
    home_page = (home_page + HOME_PAGES + (d > 0 ? 1 : -1)) % HOME_PAGES;
    dirty = 1;
}

void ui_notify(const char *title, const char *value)
{
    copy(pop_title, title, sizeof pop_title);
    copy(pop_value, value, sizeof pop_value);
    pop = POP_TEXT;
    pop_ms = 1200;
    dirty = 1;
}

void ui_focus_knob(int knob)
{
    pop = POP_KNOB;
    pop_knob = knob;
    pop_ms = 1200;
    dirty = 1;
}

void ui_learn_begin(void)
{
    dialog = DLG_LEARN;
    dirty = 1;
}

void ui_tick_ms(uint32_t ms)
{
    if (pop_ms) {
        pop_ms = ms >= pop_ms ? 0 : pop_ms - ms;
        if (!pop_ms) {
            pop = POP_NONE;
            dirty = 1;
        }
    }
    blink_ms += ms;
    if (blink_ms >= 250) {
        blink_ms = 0;
        blink ^= 1;
        if (dialog == DLG_LEARN || (depth < 0 && app_link() == LINK_HOST && host.view == VIEW_CLIPS))
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
    if (s > top[depth] + ROWS - 1) top[depth] = s - ROWS + 1;
}

static void back(void)
{
    if (dialog) { dialog = DLG_NONE; return; }
    if (editing) { editing = 0; return; }
    if (direct || depth <= 0) { go_home(); return; }
    depth--;
}

static void edit(const item_t *it, int d)
{
    int step = it->kind == K_KNOBCC || !it->step ? 1 : it->step;
    int v = get(it) + d * step;
    if (it->kind == K_ENUM && it->max - it->min == 1)
        v = d > 0 ? it->max : it->min;
    set(it, v);
    app_settings_changed();
}

static void enter(void)
{
    if (dialog == DLG_CONFIRM) {
        const item_t *it = confirm_item;
        dialog = DLG_NONE;
        it->action();
        return;
    }
    if (dialog == DLG_LEARN) {
        dialog = DLG_NONE;
        app_learn_chord();
        return;
    }
    if (dialog) { dialog = DLG_NONE; return; }
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
    case K_CONFIRM:
        confirm_item = it;
        dialog = DLG_CONFIRM;
        break;
    case K_ENUM:
        if (it->max - it->min == 1) {           /* two choices: push flips */
            set(it, get(it) == it->min ? it->max : it->min);
            app_settings_changed();
            break;
        }
        /* fall through */
    default:
        editing = !editing;
        break;
    }
}

int ui_input(uint8_t in, int16_t value)
{
    if (!ui_menu_open())
        return 0;
    dirty = 1;
    if (dialog == DLG_UPDATE)
        return 1;
    if (dialog) {
        if (in == UI_PUSH || in == UI_RIGHT) enter();
        else if (in == UI_LEFT || in == UI_BACK) back();
        return 1;
    }
    const item_t *it = &stack[depth]->items[sel[depth]];
    switch (in) {
    case UI_TURN:
        if (editing) edit(it, value);
        else move(value);
        break;
    case UI_UP: editing = 0; move(-1); break;
    case UI_DOWN: editing = 0; move(1); break;
    case UI_PUSH:
    case UI_RIGHT:
        enter();
        break;
    case UI_LEFT:
    case UI_BACK:
        back();
        break;
    }
    return 1;
}

/* ---------- rendering ---------- */

static void render_splash_lines(const char *a, const char *b)
{
    gfx_text_center(0, a, &font_huge);
    gfx_text_center(DISP_H - 8, b, &font_small);
}

void ui_splash(void)
{
    gfx_clear();
    char b[24];
    put_str(put_str(put_str(b, board_info.short_name), "  "), MK_VERSION);
    render_splash_lines("My Kontrol", b);
}

/* top status line shared by the home screens */
static void status_line(const char *badge)
{
    char b[24], *p = b;
    const settings_t *s = &settings;
    p = put_int(put_str(p, "CH"), s->channel + 1, 0);
    if (s->octave) p = put_int(put_str(p, " OCT"), s->octave, 1);
    if (s->transpose) p = put_int(put_str(p, " TR"), s->transpose, 1);
    if (s->split_on) p = put_str(p, " SPLIT");
    gfx_text(0, 0, b, &font_small);
    if (badge) {
        int w = gfx_text_width(badge, &font_small);
        gfx_text(DISP_W - w, 0, badge, &font_small);
        gfx_invert(DISP_W - w - 1, 0, w + 1, 8);
    }
}

static void render_play(void)
{
    char b[24], *p;
    const settings_t *s = &settings;
    status_line(0);

    /* centre: scale, or the last note played */
    if (s->sp.scale_mode != SCALE_OFF) {
        p = put_str(put_str(b, note_names[s->sp.root]), " ");
        copy(p, sp_scales[s->sp.scale].name, b + 15 - p);
        gfx_text(0, 9, b, &font_large);
    } else if (play_note_gen) {
        put_note(b, play_last_note);
        gfx_text(0, 9, b, &font_large);
    } else {
        gfx_text(0, 9, "My Kontrol", &font_large);
    }

    /* velocity meter on the right edge */
    if (play_note_gen) {
        int h = play_last_velocity * 22 / 127;
        gfx_fill(DISP_W - 3, 31 - h, 3, h + 1, 1);
        put_int(b, play_last_velocity, 0);
        gfx_text_right(DISP_W - 5, 17, b, &font_small);
    }

    /* bottom: what is shaping the notes */
    p = b;
    *p = 0;
    if (s->sp.repeat_on) p = put_str(put_str(p, "RPT "), sp_arp_rate_names[s->sp.arp_rate]);
    else if (s->sp.arp_on) p = put_str(put_str(p, "ARP "), sp_arp_rate_names[s->sp.arp_rate]);
    if (s->sp.repeat_on || s->sp.arp_on) {
        p = put_str(p, " ");
        p = s->sp.ext_clock ? put_str(p, "EXT") : put_bpm(p, s->sp.bpm_x10);
    }
    if (s->sp.chord != CHORD_OFF) {
        if (p != b) p = put_str(p, " ");
        p = put_str(p, sp_chord_names[s->sp.chord]);
    }
    if (s->sp.strum_ms && p - b < 18) p = put_str(p, " STRUM");
    if (p == b && !play_note_gen)
        put_str(b, "Push: menu  Turn: pages");
    gfx_text_n(0, 25, b, 25, &font_small);
}

static void render_knobs(void)
{
    char b[24], *p = put_int(put_str(b, "Knobs  Page "), settings.knob_page + 1, 0);
    (void)p;
    gfx_text(0, 0, b, &font_small);
    int w = DISP_W / BOARD_KNOBS;
    for (int k = 0; k < BOARD_KNOBS; k++) {
        int x = k * w, v = app_knob_value(settings.knob_page, k);
        int h = v * 14 / 127;
        gfx_frame(x + 2, 9, w - 4, 16);
        gfx_fill(x + 3, 24 - h, w - 6, h, 1);
        put_int(b, settings.knob_cc[settings.knob_page][k], 0);
        gfx_text((x + w / 2) - gfx_text_width(b, &font_small) / 2, 25, b, &font_small);
    }
}

/* title: what the knobs control, inverted, at the left */
static int mode_title(const char *name)
{
    int w = gfx_text(1, 0, name, &font_small);
    gfx_invert(0, 0, w + 1, 8);
    return w + 4;
}

static void render_mackie(void)
{
    char b[24];
    mode_title(knob_target_names[settings.knob_target]);
    /* position: bars.beats (or the SMPTE digits) from the timecode display */
    const char *tc = mcu_timecode;
    char *p = b;
    for (int i = 0; i < 5; i++) {
        if (tc[i] != ' ' || p != b)
            *p++ = tc[i];
        if (i == 2 && p != b)
            *p++ = mcu_led[MCU_LED_BEATS] ? '.' : ':';
    }
    *p = 0;
    gfx_text_right(DISP_W, 0, b, &font_small);
    /* 8 knobs: the DAW's name for each (LCD top row) and its ring position */
    for (int k = 0; k < 8; k++) {
        int x = (k & 3) * 32, y = k < 4 ? 8 : 20;
        gfx_text_n(x, y, &mcu_lcd[0][k * 7], 6, &font_small);
        uint8_t ring = mcu_ring[k], pos = ring & 0x0F, mode = (ring >> 4) & 3;
        if (pos)
            gfx_bar(x, y + 8, 29, 3, pos - 1, 10, mode == 1);
        else
            gfx_hline_dotted(x, y + 9, 29);
    }
}

static void knob_cells(const char names[][HOST_TEXT_LEN / 2 + 1])
{
    for (int k = 0; k < 8; k++) {
        int x = (k & 3) * 32, y = k < 4 ? 8 : 20;
        gfx_text_n(x, y, names[k], 6, &font_small);
        if (host.knob_flags[k] & 1)
            gfx_bar(x, y + 8, 29, 3, host.knob_pos[k], 127, host.knob_flags[k] & 2);
    }
}

static void render_host_device(void)
{
    char b[26];
    const char *title = host.device[0] ? host.device : host.track;
    gfx_text_n(0, 0, title[0] ? title : "No Device", 16, &font_small);
    copy(b, host.subtitle[0] ? host.subtitle : host.bank, 9);
    gfx_text_right(DISP_W, 0, b, &font_small);
    knob_cells(host.knob_name);
}

static void render_host_mixer(void)
{
    static const char *const names[VIEW_COUNT] = {"Device", "Volume", "Clips", "Pan", "Sends"};
    int x = mode_title(names[host.view]);
    char b[26];
    copy(b, host.subtitle[0] ? host.subtitle : host.track, (DISP_W - x) / 5 + 1);
    gfx_text_right(DISP_W, 0, b, &font_small);
    knob_cells(host.knob_name);
}

static void render_host_clips(void)
{
    int cr = host.cursor_row, cc = host.cursor_col;
    gfx_text_n(0, 0, cr < GRID_ROWS ? host.row[cr] : "", 12, &font_small);
    if (cc < GRID_COLS)
        gfx_text_right(DISP_W, 0, host.col[cc], &font_small);
    for (int r = 0; r < host.grid_rows; r++)
        for (int c = 0; c < host.grid_cols; c++) {
            int x = c * 16 + 2, y = 9 + r * 6, st = host.grid[r][c];
            switch (st) {
            case GRID_EMPTY: gfx_pixel(x + 6, y + 1, 1); break;
            case GRID_ARMED: gfx_fill(x + 4, y + 1, 5, 1, 1); break;
            case GRID_STOPPED: gfx_frame(x + 1, y, 10, 4); break;
            case GRID_PLAYING: gfx_fill(x + 1, y, 10, 4, 1); break;
            case GRID_RECORDING: case GRID_QUEUED: case GRID_QUEUED_REC:
                if (blink) gfx_fill(x + 1, y, 10, 4, 1); else gfx_frame(x + 1, y, 10, 4);
                break;
            }
            if (r == cr && c == cc)
                gfx_frame(x - 1, y - 1, 14, 6);
        }
}

static void render_menu(void)
{
    const menu_t *m = stack[depth];
    int tx = gfx_text(1, 0, m->title, &font_small);
    gfx_fill(tx + 3, 3, DISP_W - 3 - tx - 3, 1, 1);
    if (m->n > ROWS) {
        int h = 24 * ROWS / m->n, y = 8 + (24 - h) * top[depth] / (m->n - ROWS);
        gfx_fill(DISP_W - 2, y, 2, h, 1);
    }
    for (int r = 0; r < ROWS && top[depth] + r < m->n; r++) {
        int i = top[depth] + r, y = 8 + r * 8;
        const item_t *it = &m->items[i];
        char v[22];
        if (it->kind == K_SUB) put_str(v, ">");
        else if (it->kind == K_ACTION || it->kind == K_CONFIRM) v[0] = 0;
        else value_text(it, v);
        int w = gfx_text_width(v, &font_small);
        gfx_text_n(2, y, it->name, (DISP_W - 8 - w) / 5, &font_small);
        gfx_text(DISP_W - 5 - w, y, v, &font_small);
        if (i == sel[depth]) {
            if (editing)
                gfx_invert(DISP_W - 6 - w, y, w + 2, 8);
            else
                gfx_invert(0, y, DISP_W - 3, 8);
        }
    }
}

static void render_dialog(void)
{
    switch (dialog) {
    case DLG_CONFIRM:
        gfx_text_center(2, confirm_item->confirm, &font_small);
        gfx_text_center(14, "Push = Yes", &font_large);
        break;
    case DLG_LEARN:
        gfx_text_center(1, "Hold a chord,", &font_small);
        gfx_text_center(10, "then push", &font_small);
        gfx_text_center(22, blink ? "* Learn *" : "  Learn  ", &font_small);
        break;
    case DLG_ABOUT: {
        char b[24];
        gfx_text_center(0, "My Kontrol", &font_large);
        put_str(put_str(put_str(b, MK_VERSION), "  "), board_info.short_name);
        gfx_text_center(16, b, &font_small);
        gfx_text_center(24, "Open source, GPL-3.0", &font_small);
        break;
    }
    case DLG_UPDATE:
        gfx_text_center(0, "Update Mode", &font_large);
        gfx_text_center(18, "Ready for the", &font_small);
        gfx_text_center(25, "web installer", &font_small);
        break;
    }
}

static void render_popup(void)
{
    const char *title = pop_title, *value = pop_value;
    char t[22], v[22];
    if (pop == POP_KNOB) {
        int k = pop_knob, link = app_link();
        if (link == LINK_HOST && settings.knob_mode == KNOBS_DAW) {
            title = host.knob_name[k][0] ? host.knob_name[k] : "Not mapped";
            value = host.knob_value[k];
        } else if (link == LINK_MACKIE && settings.knob_mode == KNOBS_DAW) {
            copy(t, &mcu_lcd[0][k * 7], 8);
            copy(v, &mcu_lcd[1][k * 7], 8);
            title = t;
            value = v;
        } else {
            char *p = put_int(put_str(t, "Knob "), k + 1, 0);
            p = put_int(put_str(p, "  CC "), settings.knob_cc[settings.knob_page][k], 0);
            put_int(v, app_knob_value(settings.knob_page, k), 0);
            title = t;
            value = v;
        }
    }
    gfx_fill(0, 0, DISP_W, DISP_H, 0);
    gfx_frame(0, 0, DISP_W, DISP_H);
    gfx_text_center(1, title, &font_small);
    int n = 0;
    while (value[n]) n++;
    if (n * font_huge.w <= DISP_W - 8)
        gfx_text_center(8, value, &font_huge);
    else if (n * font_large.w <= DISP_W - 8)
        gfx_text_center(11, value, &font_large);
    else
        gfx_text_center(16, value, &font_small);
}

static int screen_changed(void)
{
    int link = app_link(), changed = 0;
    if (link != seen_link) { seen_link = link; changed = 1; }
    if (link == LINK_HOST && host.gen != seen_host) { seen_host = host.gen; changed = 1; }
    if (link == LINK_MACKIE && (mcu_lcd_gen != seen_lcd || mcu_timecode_gen != seen_tc)) {
        seen_lcd = mcu_lcd_gen;
        seen_tc = mcu_timecode_gen;
        changed = 1;
    }
    if (link == LINK_NONE && play_note_gen != seen_note) { seen_note = play_note_gen; changed = 1; }
    return changed;
}

int ui_render(void)
{
    if (screen_changed())
        dirty = 1;
    if (!dirty)
        return 0;
    dirty = 0;
    gfx_clear();
    if (dialog) {
        render_dialog();
    } else if (depth >= 0) {
        render_menu();
    } else {
        switch (app_link()) {
        case LINK_HOST:
            if (host.view == VIEW_VOLUME || host.view == VIEW_PAN || host.view == VIEW_SENDS) render_host_mixer();
            else if (host.view == VIEW_CLIPS) render_host_clips();
            else render_host_device();
            break;
        case LINK_MACKIE:
            render_mackie();
            break;
        default:
            if (home_page == HOME_KNOBS) render_knobs();
            else render_play();
            break;
        }
    }
    if (pop && dialog != DLG_UPDATE)
        render_popup();
    return 1;
}
