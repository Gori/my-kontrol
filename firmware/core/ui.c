#include "ui.h"
#include "app.h"
#include "hal.h"
#include "host.h"
#include "mcu.h"
#include "play.h"
#include "render.h"
#include "screen.h"
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
static const char *const on_off[] = {"On", "Off"};
static const char *const scale_modes[] = {"Off", "Quantize", "Easy"};
static const char *const vel_curves[] = {"Soft", "Linear", "Hard", "Fixed"};
static const char *const clock_src[] = {"Internal", "External"};
static const char *const daw_modes[] = {"Auto", "Off"};
static const char *const knob_modes[] = {"DAW", "MIDI CC"};
static const char *const tempo_lights[] = {"Auto", "Always", "Off"};
static const char *const strum_dirs[] = {"Up", "Down", "Alternate"};
static const char *const repeat_vels[] = {"Key", "Mod Strip"};
static const char *const follow_ignore[] = {"Follow", "Ignore"};
static const char *const yes_no[] = {"Yes", "No"};
static const char *const nav_modes[] = {"Off", "Auto", "Always"};
#if BOARD_MIDI_DIN
static const char *const midi_outs[] = {"USB + MIDI", "USB", "MIDI"};
#endif

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
    {"Aftertouch", K_ENUM, T_U8, &S->aftertouch, 0, 1, 1, on_off},
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
    {"M/S Mode 1", K_ENUM, T_U8, &S->knob_modes[0], 0, KM_COUNT - 1, 1, knob_target_names},
    {"M/S Mode 2", K_ENUM, T_U8, &S->knob_modes[1], 0, KM_COUNT - 1, 1, knob_target_names},
    {"M/S Mode 3", K_ENUM, T_U8, &S->knob_modes[2], 0, KM_COUNT - 1, 1, knob_target_names},
    {"M/S Mode 4", K_ENUM, T_U8, &S->knob_modes[3], 0, KM_COUNT - 1, 1, knob_target_names},
    {"M/S Mode 5", K_ENUM, T_U8, &S->knob_modes[4], 0, KM_COUNT - 1, 1, knob_target_names},
    {"Mod Strip CC", K_INT, T_U8, &S->strip_mod_cc, 0, 127, 1},
    {"Pedal Tip CC", K_INT, T_U8, &S->pedal_cc[0], 0, 127, 1},
    {"Pedal Ring CC", K_INT, T_U8, &S->pedal_cc[1], 0, 127, 1},
    {"Expression CC", K_INT, T_U8, &S->expression_cc, 0, 127, 1},
};
static const item_t preset_items[] = {
    {"Slot", K_INT, T_U8, &preset_slot, 0, 7, 1, 0, fmt_slot},
    {"Load", K_ACTION, .action = act_load},
    {"Save", K_ACTION, .action = act_save},
};
static const item_t system_items[] = {
    {"DAW Control",  K_ENUM, T_U8, &S->daw_mode, 0, DAW_MODE_COUNT - 1, 1, daw_modes},
    {"DAW Scale",    K_ENUM, T_U8, &S->daw_scale, 0, 1, 1, follow_ignore},
    {"DAW Sets View", K_ENUM, T_U8, &S->daw_view, 0, 1, 1, yes_no},
    {"Tempo Light",  K_ENUM, T_U8, &S->tempo_light, 0, TEMPO_LIGHT_COUNT - 1, 1, tempo_lights},
    {"Light Level",  K_INT,  T_U8, &S->led_dim, 0, 120, 5},
    {"Contrast",     K_INT,  T_U8, &S->contrast, 0, 255, 16},
    {"Screen Saver", K_INT,  T_U8, &S->screensaver, 0, 4, 1, 0, fmt_saver},
    {"4D as Keys",   K_ENUM, T_U8, &S->nav_keys, 0, NAV_COUNT - 1, 1, nav_modes},
#if BOARD_MIDI_DIN
    {"MIDI Out",     K_ENUM, T_U8, &S->midi_out, 0, MIDI_OUT_COUNT - 1, 1, midi_outs},
    {"MIDI Thru",    K_ENUM, T_U8, &S->midi_thru, 0, 1, 1, off_on},
#endif
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

/* items for what only some keyboards have, by the setting they change */
static const struct { const void *ptr; uint16_t cap; } item_caps[] = {
    {&S->aftertouch, CAP_AFTERTOUCH},
    {&S->expression_cc, CAP_EXPRESSION},
};

static int shown(const item_t *it)
{
    for (unsigned i = 0; i < sizeof item_caps / sizeof item_caps[0]; i++)
        if (it->ptr == item_caps[i].ptr)
            return (board_info.caps & item_caps[i].cap) != 0;
    return 1;
}

static int items_shown(const menu_t *m)
{
    int n = 0;
    for (int i = 0; i < m->n; i++)
        n += shown(&m->items[i]);
    return n;
}

static const item_t *item(const menu_t *m, int i)   /* i-th shown item */
{
    for (int k = 0; k < m->n; k++)
        if (shown(&m->items[k]) && i-- == 0)
            return &m->items[k];
    return &m->items[0];
}

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

static uint16_t seen_host, seen_lcd, seen_tc, seen_note, seen_key_note;
static uint32_t key_label_ms;           /* the label of the key just played is on screen */
static const char *key_label;
static int msg_scroll;
static uint8_t seen_msg_id;
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

void ui_message_scroll(int d)
{
    int s = msg_scroll + d;
    msg_scroll = s < 0 ? 0 : s > 60 ? 60 : s;    /* the renderer clamps to the text */
    dirty = 1;
}

void ui_tick_ms(uint32_t ms)
{
    if (key_label_ms) {
        key_label_ms = ms >= key_label_ms ? 0 : key_label_ms - ms;
        if (!key_label_ms)
            dirty = 1;
    }
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
        if (dialog == DLG_LEARN || (depth < 0 && app_link() == LINK_HOST &&
                                    (host.view == VIEW_CLIPS || (host.msg_id && (host.msg_flags & MSG_BUSY)))))
            dirty = 1;
    }
}

static void move(int d)
{
    const menu_t *m = stack[depth];
    int s = sel[depth] + d, n = items_shown(m);
    if (s < 0) s = 0;
    if (s >= n) s = n - 1;
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
    const item_t *it = item(stack[depth], sel[depth]);
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
    const item_t *it = item(stack[depth], sel[depth]);
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

/* ---------- the screen model ----------
   ui.c decides what is shown and fills a screen_t; ui/oled.c (render.h) draws
   it. String content is built here into these scratch buffers, valid until the
   renderer returns, one frame at a time. */

/* Only one base screen is built per ui_render() call, so the base screens share
   one arena. The popup overlays a base screen (both are read when the renderer
   draws), so its two buffers sit outside the union. */
static struct {
    union {
        struct { char status[24], centre[16], vel[8], hint[24]; } play;
        struct { char title[16], cc[BOARD_KNOBS][4]; } ccpage;
        char tc[12], bank[8], count[12], about[24];   /* mackie / host / list / dialog + splash */
        char val[ROWS][22];                            /* menu row values */
    } u;
    char pop_t[22], pop_v[8];                          /* knob popup */
} sb;

static void build_play(screen_t *s)
{
    const settings_t *st = &settings;
    char *p;
    s->kind = SCR_PLAY;

    p = put_int(put_str(sb.u.play.status, "CH"), st->channel + 1, 0);
    if (st->octave) p = put_int(put_str(p, " OCT"), st->octave, 1);
    if (st->transpose) p = put_int(put_str(p, " TR"), st->transpose, 1);
    if (st->split_on) p = put_str(p, " SPLIT");
    s->u.play.status = sb.u.play.status;

    if (st->sp.scale_mode != SCALE_OFF) {
        p = put_str(put_str(sb.u.play.centre, note_names[st->sp.root]), " ");
        copy(p, sp_scales[st->sp.scale].name, sb.u.play.centre + 15 - p);
        s->u.play.centre = sb.u.play.centre;
    } else if (play_note_gen) {
        put_note(sb.u.play.centre, play_last_note);
        s->u.play.centre = sb.u.play.centre;
    } else {
        s->u.play.centre = "My Kontrol";
    }

    if (play_note_gen) {
        s->u.play.has_velocity = 1;
        s->u.play.velocity = play_last_velocity;
        put_int(sb.u.play.vel, play_last_velocity, 0);
        s->u.play.vel_text = sb.u.play.vel;
    }

    p = sb.u.play.hint;
    *p = 0;
    if (st->sp.repeat_on) p = put_str(put_str(p, "RPT "), sp_arp_rate_names[st->sp.arp_rate]);
    else if (st->sp.arp_on) p = put_str(put_str(p, "ARP "), sp_arp_rate_names[st->sp.arp_rate]);
    if (st->sp.repeat_on || st->sp.arp_on) {
        p = put_str(p, " ");
        p = st->sp.ext_clock ? put_str(p, "EXT") : put_bpm(p, st->sp.bpm_x10);
    }
    if (st->sp.chord != CHORD_OFF) {
        if (p != sb.u.play.hint) p = put_str(p, " ");
        p = put_str(p, sp_chord_names[st->sp.chord]);
    }
    if (st->sp.strum_ms && p - sb.u.play.hint < 18) p = put_str(p, " STRUM");
    if (p == sb.u.play.hint && !play_note_gen)
        put_str(sb.u.play.hint, "Push: menu  Turn: pages");
    s->u.play.hint = sb.u.play.hint;
}

static void build_ccpage(screen_t *s)
{
    s->kind = SCR_CCPAGE;
    char *p = put_int(put_str(sb.u.ccpage.title, "Knobs  Page "), settings.knob_page + 1, 0);
    (void)p;
    s->u.ccpage.title = sb.u.ccpage.title;
    s->u.ccpage.n = BOARD_KNOBS;
    for (int k = 0; k < BOARD_KNOBS; k++) {
        s->u.ccpage.knob[k].value = app_knob_value(settings.knob_page, k);
        put_int(sb.u.ccpage.cc[k], settings.knob_cc[settings.knob_page][k], 0);
        s->u.ccpage.knob[k].cc = sb.u.ccpage.cc[k];
    }
}

static const char *host_title(void)
{
    static const char *const names[VIEW_COUNT] = {"Plug-in", "Volume", "Clips", "Pan", "Sends", "Instrument"};
    const char *n = host_mode_name(host.view);
    return n ? n : host.view < VIEW_COUNT ? names[host.view] : "Host";
}

/* top right: the device / extra text, or the label of the key just played */
static void build_host_right(screen_t *s, int device)
{
    const char *d = host.device, *extra = host.subtitle;
    if (device && !d[0])
        d = "None";
    if (!device && (host.proto < 2 || !d[0]))   /* mixer: v1 hosts leave the device name behind */
        d = host.subtitle[0] ? host.subtitle : host.track, extra = "";
    if (host.page_banks > 1) {         /* v2: "2/6" */
        char *q = put_int(sb.u.bank, host.page_bank + 1, 0);
        *q++ = '/';
        put_int(q, host.page_banks, 0);
        extra = sb.u.bank;
    }
    if (key_label_ms && key_label)
        d = key_label, extra = "";
    s->u.knobs.right_a = d;
    s->u.knobs.right_b = extra;
    s->u.knobs.room_limit = 1;
}

static void build_host_slots(screen_t *s)
{
    int meters = host.features & HF_METERS;
    for (int k = 0; k < 8; k++) {
        knob_slot_t *sl = &s->u.knobs.slot[k];
        sl->name = host.knob_name[k];
        sl->name_len = 6;
        sl->value = host.knob_value[k];
        if (host.knob_flags[k] & 1) {
            sl->bar_style = BAR_FILL;
            sl->bar = host.knob_pos[k];
            sl->bar_max = 127;
            sl->bipolar = host.knob_flags[k] & 2;
        }
        if (meters && host.meter[k])
            sl->meter = host.meter[k];
    }
}

static void build_host_device(screen_t *s)
{
    s->kind = SCR_KNOBS;
    s->u.knobs.title = host_title();
    build_host_right(s, 1);
    build_host_slots(s);
    if (host.features & HF_METERS)
        s->u.knobs.channel_meter =
            host.channel_meter[0] > host.channel_meter[1] ? host.channel_meter[0] : host.channel_meter[1];
}

static void build_host_mixer(screen_t *s)
{
    s->kind = SCR_KNOBS;
    s->u.knobs.title = host_title();
    build_host_right(s, 0);
    build_host_slots(s);
}

static void build_mackie(screen_t *s)
{
    s->kind = SCR_KNOBS;
    s->u.knobs.title = knob_target_names[settings.knob_target];
    /* position: bars.beats (or the SMPTE digits) from the timecode display */
    const char *tc = mcu_timecode;
    char *p = sb.u.tc;
    for (int i = 0; i < 5; i++) {
        if (tc[i] != ' ' || p != sb.u.tc)
            *p++ = tc[i];
        if (i == 2 && p != sb.u.tc)
            *p++ = mcu_led[MCU_LED_BEATS] ? '.' : ':';
    }
    *p = 0;
    s->u.knobs.right_a = sb.u.tc;
    s->u.knobs.right_b = "";
    for (int k = 0; k < 8; k++) {
        knob_slot_t *sl = &s->u.knobs.slot[k];
        sl->name = &mcu_lcd[0][k * 7];
        sl->name_len = 6;
        sl->value = &mcu_lcd[1][k * 7];
        uint8_t ring = mcu_ring[k], pos = ring & 0x0F, mode = (ring >> 4) & 3;
        if (pos) {
            sl->bar_style = BAR_FILL;
            sl->bar = pos - 1;
            sl->bar_max = 10;
            sl->bipolar = mode == 1;
        } else {
            sl->bar_style = BAR_DOTTED;
        }
    }
}

static const host_row_t *cached_row(uint16_t index)
{
    for (int i = 0; i < HOST_LIST_CACHE; i++)
        if (host.rows[i].index == index)
            return &host.rows[i];
    return 0;
}

static void build_host_list(screen_t *s)
{
    s->kind = SCR_LIST;
    s->u.list.title = host.list_title[0] ? host.list_title : host_title();
    s->u.list.total = host.list_total;
    s->u.list.cursor = host.list_cursor;
    s->u.list.first = host.list_first;
    if (host.list_total) {
        char *p = put_int(sb.u.count, host.list_cursor + 1, 0);
        *p++ = '/';
        put_int(p, host.list_total, 0);
        s->u.list.count = sb.u.count;
    }
    for (int r = 0; r < HOST_LIST_ROWS; r++) {
        uint16_t i = host.list_first + r;
        const host_row_t *row = i < host.list_total ? cached_row(i) : 0;
        s->u.list.row[r].present = row != 0;
        s->u.list.row[r].flags = row ? row->flags : 0;
        s->u.list.row[r].text = row ? row->text : 0;
    }
}

static void build_host_message(screen_t *s)
{
    s->kind = SCR_MESSAGE;
    s->u.message.text = host.msg;
    s->u.message.scroll = &msg_scroll;
    s->u.message.busy_dot = (host.msg_flags & MSG_BUSY) && blink;
}

static void build_clips(screen_t *s)
{
    int cr = host.cursor_row, cc = host.cursor_col;
    s->kind = SCR_CLIPS;
    s->u.clips.row_label = cr < GRID_ROWS ? host.row[cr] : "";
    s->u.clips.row_label_len = 12;
    s->u.clips.col_label = cc < GRID_COLS ? host.col[cc] : 0;
    s->u.clips.rows = host.grid_rows;
    s->u.clips.cols = host.grid_cols;
    s->u.clips.cr = cr;
    s->u.clips.cc = cc;
    s->u.clips.blink = blink;
    for (int r = 0; r < host.grid_rows; r++)
        for (int c = 0; c < host.grid_cols; c++)
            s->u.clips.cell[r][c] = host.grid[r][c];
}

static void build_menu(screen_t *s)
{
    const menu_t *m = stack[depth];
    int n = items_shown(m), nrow = 0;
    s->kind = SCR_MENU;
    s->u.menu.title = m->title;
    s->u.menu.count = n;
    s->u.menu.top = top[depth];
    s->u.menu.sel = sel[depth];
    s->u.menu.editing = editing;
    for (int r = 0; r < ROWS && top[depth] + r < n; r++) {
        const item_t *it = item(m, top[depth] + r);
        if (it->kind == K_SUB) s->u.menu.row[r].value = ">";
        else if (it->kind == K_ACTION || it->kind == K_CONFIRM) s->u.menu.row[r].value = "";
        else { value_text(it, sb.u.val[r]); s->u.menu.row[r].value = sb.u.val[r]; }
        s->u.menu.row[r].name = it->name;
        nrow++;
    }
    s->u.menu.nrow = nrow;
}

static void build_dialog(screen_t *s)
{
    s->kind = SCR_DIALOG;
    s->u.dialog.id = dialog;            /* DLG_* line up with SCR_DLG_* */
    s->u.dialog.blink = blink;
    if (dialog == DLG_CONFIRM)
        s->u.dialog.confirm = confirm_item->confirm;
    if (dialog == DLG_ABOUT) {
        put_str(put_str(put_str(sb.u.about, MK_VERSION), "  "), board_info.short_name);
        s->u.dialog.about = sb.u.about;
    }
}

static void build_popup(screen_t *s)
{
    const char *title = pop_title, *value = pop_value;
    if (pop == POP_KNOB) {
        int k = pop_knob, link = app_link();
        if (link == LINK_HOST && settings.knob_mode == KNOBS_DAW) {
            title = host.knob_name[k][0] ? host.knob_name[k] : "Not mapped";
            if (host.group[k][0] && host.knob_name[k][0]) {   /* "Filter Cutoff" */
                char *p = put_str(sb.pop_t, host.group[k]);
                *p++ = ' ';
                copy(p, host.knob_name[k], sizeof sb.pop_t - (p - sb.pop_t));
                title = sb.pop_t;
            }
            value = host.knob_value[k];
        } else if (link == LINK_MACKIE && settings.knob_mode == KNOBS_DAW) {
            copy(sb.pop_t, &mcu_lcd[0][k * 7], 8);
            copy(sb.pop_v, &mcu_lcd[1][k * 7], 8);
            title = sb.pop_t;
            value = sb.pop_v;
        } else {
            char *p = put_int(put_str(sb.pop_t, "Knob "), k + 1, 0);
            p = put_int(put_str(p, "  CC "), settings.knob_cc[settings.knob_page][k], 0);
            put_int(sb.pop_v, app_knob_value(settings.knob_page, k), 0);
            title = sb.pop_t;
            value = sb.pop_v;
        }
    }
    s->popup.show = 1;
    s->popup.title = title;
    s->popup.value = value;
}

void ui_splash(void)
{
    screen_t s = {0};
    s.kind = SCR_SPLASH;
    put_str(put_str(put_str(sb.u.about, board_info.short_name), "  "), MK_VERSION);
    s.u.splash.a = "My Kontrol";
    s.u.splash.b = sb.u.about;
    ui_renderer.show(&s);
}

static int screen_changed(void)
{
    int link = app_link(), changed = 0;
    if (link != seen_link) { seen_link = link; changed = 1; }
    if (link == LINK_HOST && host.gen != seen_host) { seen_host = host.gen; changed = 1; }
    if (link == LINK_HOST && play_note_gen != seen_key_note) {   /* drum lane names */
        seen_key_note = play_note_gen;
        const char *l = host_key_label(play_last_note);
        if (l) {
            key_label = l;
            key_label_ms = 1500;
            changed = 1;
        }
    }
    if (host.msg_id != seen_msg_id) {
        seen_msg_id = host.msg_id;
        msg_scroll = 0;
    }
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
    screen_t s = {0};
    if (dialog) {
        build_dialog(&s);
    } else if (depth >= 0) {
        build_menu(&s);
    } else {
        switch (app_link()) {
        case LINK_HOST:
            if (host.msg_id) build_host_message(&s);
            else if (host.list_id) build_host_list(&s);
            else if (host.view == VIEW_VOLUME || host.view == VIEW_PAN || host.view == VIEW_SENDS) build_host_mixer(&s);
            else if (host.view == VIEW_CLIPS) build_clips(&s);
            else build_host_device(&s);
            if (host.lost)                  /* the host went quiet: say so over the top line */
                s.lost = 1;
            break;
        case LINK_MACKIE:
            build_mackie(&s);
            break;
        default:
            if (home_page == HOME_KNOBS) build_ccpage(&s);
            else build_play(&s);
            break;
        }
    }
    if (pop && dialog != DLG_UPDATE)
        build_popup(&s);
    ui_renderer.show(&s);
    return 1;
}
