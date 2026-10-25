/* Routes board events to Smart Play, the DAW links and the UI; owns the
   button lights. Three ways to run:

   LINK_NONE    no DAW: every button acts on the keyboard itself
   LINK_MACKIE  a DAW speaks Mackie Control on the DAW port (Logic, Live, ...)
   LINK_HOST    the My Kontrol script runs in the DAW (docs/protocol.md) */
#include "app.h"
#include "gfx.h"
#include "host.h"
#include "kbd.h"
#include "lightguide.h"
#include "mcu.h"
#include "midi.h"
#include "play.h"
#include "settings.h"
#include "settings_store.h"
#include "smartplay.h"
#include "ui.h"

static uint8_t shift, usb_up;
static uint8_t knob_val[KNOB_PAGES][BOARD_KNOBS];
static uint32_t now_ms, last_ms, idle_ms;
static uint8_t screen_off;
static uint8_t clock_port = 0xFF;          /* port the external clock arrives on */
static uint32_t clock_seen_ms;
static uint8_t tap_flash;                  /* tap tempo turned the Tempo light on */
static uint32_t beat_ms;                   /* when the last DAW beat arrived */
static uint16_t seen_beat_gen;
static char seen_beats[2];

static void daw_send(const uint8_t *b, uint16_t n)
{
    if (usb_up)
        midi_send(PORT_DAW, b, n);
}

int app_link(void)
{
    if (settings.daw_mode == DAW_OFF)
        return LINK_NONE;
    if (host.connected)
        return LINK_HOST;
    if (mcu_connected)
        return LINK_MACKIE;
    return LINK_NONE;
}

int app_shift(void) { return shift; }
uint8_t app_knob_value(int page, int knob) { return knob_val[page & (KNOB_PAGES - 1)][knob % BOARD_KNOBS]; }

static void daw_scale(void);

/* Where the keys go: USB while it is up, and the 5-pin per System > MIDI
   Out. Changing MIDI Out ends the notes where they started. */
static void keys_route(void)
{
#if BOARD_MIDI_DIN
    static uint8_t out = 0xFF;
    if (out != 0xFF && out != settings.midi_out)
        play_panic();
    out = settings.midi_out;
    midi_keys_route((usb_up && out != MIDI_OUT_DIN ? MIDI_TO_USB : 0) | (out != MIDI_OUT_USB ? MIDI_TO_DIN : 0));
#else
    midi_keys_route(usb_up ? MIDI_TO_USB : 0);
#endif
}

void app_settings_changed(void)
{
    settings_sanitize(&settings);
    keys_route();
    daw_scale();
    play_settings_changed();
    board_display_contrast(settings.contrast);
    settings_touch();
    ui_invalidate();
}

void app_panic(void)
{
    play_panic();
}

void app_enter_bootloader(void)
{
    play_panic();
    if (settings_pending())
        settings_save_now();
    /* show the update screen, then let USB send what is queued */
    ui_render();
#if !BOARD_SEG_DISPLAYS                 /* a segment renderer has sent its frame already */
    for (int p = 0; p < DISP_H / 8; p++)
        board_display_page(p, &gfx_fb[p * DISP_W]);
#endif
    hal_delay_ms(20);
    hal_enter_bootloader();
}

void app_usb_mounted(int mounted)
{
    usb_up = mounted;
    keys_route();
    if (!mounted) {
        mcu_reset();
        host_usb_reset();
    }
}

/* ---------- MIDI in ---------- */

#if BOARD_MIDI_DIN
/* With Clock Out on the internal tempo, our clock goes out the 5-pin and
   MIDI Thru keeps the incoming one back (two clocks would fight). */
static int own_clock(uint8_t b)
{
    return settings.clock_out && !settings.sp.ext_clock && (b == 0xF8 || (b >= 0xFA && b <= 0xFC));
}

/* 5-pin in goes to the computer on EXT, and with MIDI Thru back out the
   5-pin, merged with the keys; EXT from the computer goes out the 5-pin. */
static void din_forward(uint8_t port, const uint8_t *m, uint16_t n)
{
    if (port == PORT_EXT) {
        midi_send(PORT_DIN, m, n);
        return;
    }
    if (usb_up)
        midi_send(PORT_EXT, m, n);
    if (settings.midi_thru && !own_clock(m[0]))
        midi_send(PORT_DIN, m, n);
}

/* sysex the same way, a byte at a time, so any length passes */
void app_midi_sysex_byte(uint8_t port, uint8_t b)
{
    if (port == PORT_EXT) {
        midi_din_sysex(PORT_EXT, b);
        return;
    }
    if (usb_up)
        midi_usb_sysex(PORT_EXT, b);
    if (settings.midi_thru)
        midi_din_sysex(PORT_DIN, b);
}
#endif

void app_midi_message(uint8_t port, uint8_t st, uint8_t d0, uint8_t d1)
{
#if BOARD_MIDI_DIN
    if (port == PORT_EXT || port == PORT_DIN) {
        uint8_t m[3] = {st, d0, d1};
        din_forward(port, m, midi_msg_len(st));
        return;                             /* notes from the 5-pin are not played */
    }
#endif
    if (port == PORT_DAW) {
        if ((st & 0x0F) == 0x0F && host.connected)
            return;                         /* channel 16 is the host protocol's own */
        switch (st & 0xF0) {
        case 0x90: mcu_note(d0, d1); break;
        case 0x80: mcu_note(d0, 0); break;
        case 0xB0: mcu_cc(d0, d1); break;
        }
        return;
    }
    /* Keys port: knob CCs echoed back by the DAW keep the knobs in sync */
    if ((st & 0xF0) == 0xB0 && (st & 0x0F) == play_channel())
        for (int p = 0; p < KNOB_PAGES; p++)
            for (int k = 0; k < BOARD_KNOBS; k++)
                if (settings.knob_cc[p][k] == d0)
                    knob_val[p][k] = d1;
}

void app_midi_sysex(uint8_t port, const uint8_t *m, uint16_t n)
{
    if (port != PORT_DAW)
        return;
    if (!host_sysex(m, n))
        mcu_sysex(m, n);
}

void app_midi_realtime(uint8_t port, uint8_t b)
{
#if BOARD_MIDI_DIN
    if (port == PORT_EXT || port == PORT_DIN)
        din_forward(port, &b, 1);
    if (port == PORT_EXT)
        return;                             /* the computer's clock for the 5-pin, not ours */
#endif
    /* follow one source: a DAW may send clock to both ports, a 5-pin clock too */
    if (clock_port != port && now_ms - clock_seen_ms < 1000)
        return;
    if (b == 0xF8) {
        clock_port = port;
        clock_seen_ms = now_ms;
    }
    switch (b) {
    case 0xF8: sp_midi_clock(); break;
    case 0xFA: sp_midi_start(); break;
    case 0xFB: sp_midi_continue(); break;
    case 0xFC: sp_midi_stop(); break;
    }
}

/* ---------- knobs ---------- */

static void knob_turned(uint8_t k, int d)
{
    int link = app_link();
    if (link != LINK_NONE && settings.knob_mode == KNOBS_DAW) {
        if (link == LINK_HOST)
            host_knob(k, d);
        else
            mcu_vpot(k, d);
        ui_focus_knob(k);
        return;
    }
    uint8_t *v = &knob_val[settings.knob_page][k];
    int x = *v + d;
    x = x < 0 ? 0 : x > 127 ? 127 : x;
    if (x == *v)
        return;
    *v = x;
    midi_keys3(0xB0 | play_channel(), settings.knob_cc[settings.knob_page][k], x);
    ui_focus_knob(k);
}

static void knob_touched(uint8_t k, int touched)
{
    int link = app_link();
    if (link == LINK_HOST && settings.knob_mode == KNOBS_DAW)
        host_knob_touch(k, touched);
    if (touched)
        ui_focus_knob(k);
}

/* ---------- buttons ---------- */

/* Mackie Control keys with their Shift variants. Mute, Solo, Plug-in and
   Track are knob-mode buttons and handled in knob_target_* below. */
static const struct { uint8_t ctl, mcu, mcu_shift; } mcu_keys[] = {
    {CTL_UNDO,        MCU_UNDO,       MCU_UNDO},        /* Shift = redo: MCU Shift held */
    {CTL_QUANTIZE,    MCU_F1,         MCU_F1 + 4},
    {CTL_IDEAS,       MCU_F1 + 1,     MCU_F1 + 5},
    {CTL_LOOP,        MCU_CYCLE,      MCU_CYCLE},
    {CTL_METRO,       MCU_CLICK,      MCU_CLICK},
    {CTL_PLAY,        MCU_PLAY,       MCU_REWIND},      /* Shift = back to start */
    {CTL_REC,         MCU_RECORD,     MCU_RECORD},
    {CTL_STOP,        MCU_STOP,       MCU_STOP},
    {CTL_PRESET_UP,   MCU_BANK_LEFT,  MCU_CH_LEFT},     /* page the knobs by 8 (Shift: by 1) */
    {CTL_PRESET_DOWN, MCU_BANK_RIGHT, MCU_CH_RIGHT},
    {CTL_MUTE,        MCU_MUTE,       MCU_MUTE},        /* Shift only: strip 1 of the bank */
    {CTL_SOLO,        MCU_SOLO,       MCU_SOLO},
    {CTL_NAV_UP,      MCU_UP,         MCU_UP},          /* previous / next track */
    {CTL_NAV_DOWN,    MCU_DOWN,       MCU_DOWN},
    {CTL_NAV_LEFT,    MCU_LEFT,       MCU_BANK_LEFT},
    {CTL_NAV_RIGHT,   MCU_RIGHT,      MCU_BANK_RIGHT},
    {CTL_NAV_PUSH,    MCU_ENTER,      MCU_ENTER},
};
#define MCU_KEYS (sizeof mcu_keys / sizeof mcu_keys[0])
static uint8_t mcu_sent[MCU_KEYS];        /* note sent on press, released on release */

/* ---------- knob modes: what the knobs control in a DAW ---------- */

static const uint8_t target_view[KM_COUNT] = {
    VIEW_INSTRUMENT, VIEW_INSTRUMENT, VIEW_DEVICE, VIEW_VOLUME, VIEW_PAN, VIEW_SENDS,
};
static const uint8_t target_assign[KM_COUNT] = {
    0, MCU_ASSIGN_INSTR, MCU_ASSIGN_PLUGIN, MCU_ASSIGN_PAN, MCU_ASSIGN_PAN, MCU_ASSIGN_SEND,
};
static uint8_t sync_tries;                 /* Mackie: presses left to reach the target */
static uint32_t sync_ms;

static void knob_target_set(uint8_t t, int announce)
{
    settings.knob_target = t;
    settings_touch();
    sync_tries = 4;
    sync_ms = 0;
    const char *name = 0;
    if (app_link() == LINK_HOST) {
        host_set_view(target_view[t]);
        name = host_mode_name(target_view[t]);
    }
    if (announce)
        ui_notify("Knobs", name ? name : knob_target_names[t]);
    ui_invalidate();
}

/* host link: any view, standard (remembered as the knob target) or one the
   host defined (MODES) */
static void view_select(uint8_t view, int announce)
{
    for (int t = KM_INSTRUMENT; t < KM_COUNT; t++)
        if (target_view[t] == view) {
            knob_target_set(t, announce);
            return;
        }
    host_set_view(view);
    if (announce) {
        const char *name = host_mode_name(view);
        ui_notify("Knobs", name ? name : view == VIEW_CLIPS ? "Clips" : "Host");
    }
    ui_invalidate();
}

/* host link: the views M / S step through: the user's order, with only what
   the host offers and has enabled, then the host's own modes */
static int host_views(uint8_t *v)
{
    int n = 0;
    for (int i = 0; i < KNOB_MODE_SLOTS; i++) {
        uint8_t t = settings.knob_modes[i];
        if (t != KM_OFF && host_mode_enabled(target_view[t]))
            v[n++] = target_view[t];
    }
    for (int i = 0; i < host.n_modes; i++)
        if (host.mode_id[i] >= VIEW_HOST && (host.mode_flags[i] & 1))
            v[n++] = host.mode_id[i];
    return n;
}

static void knob_target_step(int d)
{
    if (app_link() == LINK_HOST) {
        uint8_t views[KNOB_MODE_SLOTS + HOST_MODES];
        int n = host_views(views), cur = -1;
        for (int i = 0; i < n; i++)
            if (views[i] == host.view)
                cur = i;
        if (n)
            view_select(views[cur < 0 ? (d > 0 ? 0 : n - 1) : (cur + d + n) % n], 1);
        return;
    }
    int n = KNOB_MODE_SLOTS, cur = -1;
    for (int i = 0; i < n; i++)
        if (settings.knob_modes[i] == settings.knob_target)
            cur = i;
    for (int k = 1; k <= n; k++) {
        int i = ((cur < 0 ? 0 : cur) + d * k + 2 * n) % n;
        uint8_t t = settings.knob_modes[i];
        if (t == KM_OFF)
            continue;
        knob_target_set(t, 1);
        return;
    }
}

/* Mackie: press the assignment (and Flip for volume) until the DAW's LEDs
   say the knobs are where we want them. A few tries only: a DAW that does
   not light these LEDs is not pressed forever. */
static void knob_target_sync(void)
{
    if (app_link() != LINK_MACKIE || !sync_tries || now_ms - sync_ms < 300)
        return;
    uint8_t t = settings.knob_target, a = target_assign[t];
    int want_flip = t == KM_VOLUME, flipped = mcu_led[MCU_FLIP] != 0;
    if (!mcu_led[a]) {
        if (flipped) {
            mcu_button(MCU_FLIP, 1);
            mcu_button(MCU_FLIP, 0);
        }
        mcu_button(a, 1);
        mcu_button(a, 0);
    } else if (want_flip != flipped) {
        mcu_button(MCU_FLIP, 1);
        mcu_button(MCU_FLIP, 0);
    } else {
        sync_tries = 0;
        return;
    }
    sync_tries--;
    sync_ms = now_ms;
}

/* M / S step the knob mode, Plug-in and Track jump to instrument and
   volume. Returns 1 if the button was one of them. */
static int knob_mode_button(uint8_t ctl, int down)
{
    if (ctl != CTL_MUTE && ctl != CTL_SOLO && ctl != CTL_PLUGIN && ctl != CTL_TRACK)
        return 0;
    if (shift && (ctl == CTL_MUTE || ctl == CTL_SOLO))
        return 0;                          /* Shift + M / S: mute / solo */
    if (!down)
        return 1;
    if (ctl == CTL_MUTE || ctl == CTL_SOLO) {
        knob_target_step(ctl == CTL_SOLO ? 1 : -1);
        return 1;
    }
    uint8_t t = ctl == CTL_PLUGIN ? (shift ? KM_PLUGIN : KM_INSTRUMENT) : (shift ? KM_SENDS : KM_VOLUME);
    if (app_link() == LINK_HOST && host_mode_index(target_view[t]) < 0)
        ui_notify(knob_target_names[t], "Not in this DAW");
    else
        knob_target_set(t, 1);
    return 1;
}

static int mackie_button(uint8_t ctl, int down)
{
    for (unsigned i = 0; i < MCU_KEYS; i++) {
        if (mcu_keys[i].ctl != ctl)
            continue;
        if (down) {
            uint8_t note = shift ? mcu_keys[i].mcu_shift : mcu_keys[i].mcu;
            mcu_sent[i] = note | (shift && ctl == CTL_UNDO ? 0x80 : 0);
            if (mcu_sent[i] & 0x80)
                mcu_button(MCU_SHIFT, 1);
            mcu_button(note, 1);
        } else if (mcu_sent[i]) {
            mcu_button(mcu_sent[i] & 0x7F, 0);
            if (mcu_sent[i] & 0x80)
                mcu_button(MCU_SHIFT, 0);
            mcu_sent[i] = 0;
        }
        return 1;
    }
    return 0;
}

static void notify_int(const char *title, int v, int sign)
{
    char b[8];
    char *p = b;
    int u = v < 0 ? -v : v;
    if (v < 0) *p++ = '-';
    else if (sign && v > 0) *p++ = '+';
    if (u >= 100) *p++ = '0' + u / 100;
    if (u >= 10) *p++ = '0' + u / 10 % 10;
    *p++ = '0' + u % 10;
    *p = 0;
    ui_notify(title, b);
}

static uint32_t taps[4];
static uint8_t tap_n;

static void tap_tempo(void)
{
    if (tap_n && now_ms - taps[(tap_n - 1) & 3] > 2000)
        tap_n = 0;                          /* a pause starts a new measurement */
    taps[tap_n & 3] = now_ms;
    tap_n++;
    tap_flash = 1;
    if (tap_n < 2) {
        ui_notify("Tap Tempo", "...");
        return;
    }
    int n = tap_n < 4 ? tap_n : 4;
    uint32_t interval = (now_ms - taps[(tap_n - n) & 3]) / (n - 1);
    if (interval < 250 || interval > 1500)  /* 40..240 BPM, the menu's range */
        return;
    settings.sp.bpm_x10 = (600000 + interval / 2) / interval;
    char v[8];
    int b = settings.sp.bpm_x10;
    char *p = v;
    if (b >= 1000) *p++ = '0' + b / 1000;
    *p++ = '0' + b / 100 % 10;
    *p++ = '0' + b / 10 % 10;
    *p++ = '.';
    *p++ = '0' + b % 10;
    *p = 0;
    ui_notify(settings.sp.ext_clock ? "Tempo (ext clock)" : "Tempo", v);
    app_settings_changed();
}

static uint8_t oct_held[2], oct_combo;

static void octave_button(int up, int down)
{
    oct_held[up] = down;
    if (!down) {
        if (!oct_held[0] && !oct_held[1])
            oct_combo = 0;
        return;
    }
    if (oct_held[!up]) {                    /* both: back to the middle, all notes off */
        oct_combo = 1;
        settings.octave = settings.transpose = 0;
        play_panic();
        ui_notify("Octave & Transpose", "Reset");
        app_settings_changed();
        return;
    }
    if (shift && !up) {
        if (settings.vel_curve == VEL_FIXED) {
            settings.vel_curve = settings.vel_curve_prev;
        } else {
            settings.vel_curve_prev = settings.vel_curve;
            settings.vel_curve = VEL_FIXED;
        }
        ui_notify("Fixed Velocity", settings.vel_curve == VEL_FIXED ? "On" : "Off");
        app_settings_changed();
        return;
    }
    if (shift && up) {
        settings.split_on = !settings.split_on;
        ui_notify("Split", settings.split_on ? "On" : "Off");
        app_settings_changed();
        return;
    }
    int o = settings.octave + (up ? 1 : -1);
    if (o < -3 || o > 3)
        return;
    settings.octave = o;
    notify_int("Octave", o, 1);
    app_settings_changed();
}

/* No DAW: Quantize = note repeat (tap toggles, hold is momentary),
   Ideas = chord memory (hold Ideas and play a chord to learn it, tap toggles) */
static uint32_t repeat_down_ms, ideas_down_ms;
static uint8_t repeat_was_on, learning;

int app_learning(void) { return learning; }

static const char *chord_name(void)
{
    static const struct { uint8_t n; int8_t iv[5]; const char *name; } names[] = {
        {3, {0, 4, 7}, "Major"}, {3, {0, 3, 7}, "Minor"}, {3, {0, 3, 6}, "Dim"},
        {3, {0, 4, 8}, "Aug"}, {3, {0, 2, 7}, "Sus2"}, {3, {0, 5, 7}, "Sus4"},
        {4, {0, 4, 7, 11}, "Maj7"}, {4, {0, 3, 7, 10}, "Min7"}, {4, {0, 4, 7, 10}, "Dom7"},
        {4, {0, 3, 6, 10}, "Min7b5"}, {4, {0, 3, 6, 9}, "Dim7"}, {2, {0, 7}, "Power"},
        {4, {0, 4, 7, 9}, "Maj6"}, {4, {0, 3, 7, 9}, "Min6"}, {4, {0, 4, 7, 14}, "Add9"},
    };
    const sp_config_t *c = &settings.sp;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (names[i].n != c->chord_mem_n)
            continue;
        int ok = 1;
        for (int k = 0; k < c->chord_mem_n; k++)
            ok &= names[i].iv[k] == c->chord_mem[k];
        if (ok)
            return names[i].name;
    }
    static char b[10] = "0 Notes";
    b[0] = '0' + c->chord_mem_n;
    return b;
}

void app_learn_chord(void)
{
    if (sp_learn_chord() == 0) {
        settings.sp.chord = CHORD_MEMORY;
        ui_notify("Chord Learned", chord_name());
        app_settings_changed();
    } else {
        ui_notify("Learn Chord", "Hold 2+ keys");
    }
}

static void local_button(uint8_t ctl, int down)
{
    switch (ctl) {
    case CTL_QUANTIZE:
        if (down) {
            repeat_down_ms = now_ms;
            repeat_was_on = settings.sp.repeat_on;
            settings.sp.repeat_on = !repeat_was_on;
            ui_notify("Note Repeat", settings.sp.repeat_on ? sp_arp_rate_names[settings.sp.arp_rate] : "Off");
            app_settings_changed();
        } else if (!repeat_was_on && settings.sp.repeat_on && now_ms - repeat_down_ms > 400) {
            settings.sp.repeat_on = 0;      /* held: momentary */
            app_settings_changed();
        }
        return;
    case CTL_IDEAS:
        if (down) {
            ideas_down_ms = now_ms;
            learning = 1;
            ui_notify("Learn Chord", "Play a chord");
        } else if (learning) {
            learning = 0;
            if (sp_held_count() >= 2) {
                app_learn_chord();
            } else if (now_ms - ideas_down_ms < 400) {
                settings.sp.chord = settings.sp.chord == CHORD_MEMORY ? CHORD_OFF : CHORD_MEMORY;
                ui_notify("Chord Memory", settings.sp.chord == CHORD_MEMORY ? chord_name() : "Off");
                app_settings_changed();
            }
        }
        return;
    case CTL_PRESET_UP:
    case CTL_PRESET_DOWN:
    case CTL_MUTE:
    case CTL_SOLO:
        if (down) {
            int up = ctl == CTL_PRESET_UP || ctl == CTL_SOLO;
            settings.knob_page = (settings.knob_page + (up ? 1 : KNOB_PAGES - 1)) % KNOB_PAGES;
            notify_int("Knob Page", settings.knob_page + 1, 0);
            app_settings_changed();
        }
        return;
    case CTL_PLAY:
    case CTL_STOP:
        /* with Clock Out on, Play / Stop start and stop whatever follows us */
        if (down && settings.clock_out && !settings.sp.ext_clock) {
            uint8_t b = ctl == CTL_PLAY ? 0xFA : 0xFC;
            midi_keys(&b, 1);
            ui_notify("MIDI Clock", ctl == CTL_PLAY ? "Start" : "Stop");
        }
        return;
    case CTL_NAV_LEFT:
    case CTL_NAV_RIGHT:
        if (down) {
            int t = settings.transpose + (ctl == CTL_NAV_RIGHT ? 1 : -1);
            if (t >= -12 && t <= 12) {
                settings.transpose = t;
                notify_int("Transpose", t, 1);
                app_settings_changed();
            }
        }
        return;
    case CTL_NAV_UP:
    case CTL_NAV_DOWN:
        if (down)
            ui_home_step(ctl == CTL_NAV_DOWN ? 1 : -1);
        return;
    case CTL_NAV_PUSH:
        if (down)
            ui_toggle_menu();
        return;
    }
}

static int is_nav(uint8_t ctl)
{
    return ctl >= CTL_NAV_UP && ctl <= CTL_NAV_PUSH;
}

/* 4D as Keys: Auto leaves the 4D to a host that handles it (HELLO features) */
static int nav_keys(int link)
{
    if (link == LINK_NONE || settings.nav_keys == NAV_OFF)
        return 0;
    return settings.nav_keys == NAV_ALWAYS || link != LINK_HOST || !(host.features & HF_ENCODER);
}

static void button(uint8_t ctl, int down)
{
    int link = app_link();
    if (ctl == CTL_SHIFT) {
        shift = down;
        if (link == LINK_HOST)
            host_button(ctl, down);
        return;
    }
    if (ctl == CTL_PEDAL_PLUG) {
        if (!down)
            play_pedal_unplugged();
        return;
    }

    /* the menu owns the 4D controls while it is open, and Back and Enter */
    if (is_nav(ctl) && ui_menu_open()) {
        static const uint8_t map[] = {UI_UP, UI_LEFT, UI_RIGHT, UI_DOWN, UI_PUSH};
        if (down)
            ui_input(map[ctl - CTL_NAV_UP], 0);
        return;
    }
    if ((ctl == CTL_BACK || ctl == CTL_ENTER) && ui_menu_open()) {
        if (down)
            ui_input(ctl == CTL_BACK ? UI_BACK : UI_PUSH, 0);
        return;
    }
    /* a list or message screen from the host takes the 4D */
    if (is_nav(ctl) && link == LINK_HOST && host_modal()) {
        host_button(ctl, down);
        return;
    }
    /* with a DAW, the 4D navigates the focused window as a keyboard:
       arrows (Shift extends the selection), push = Return, Shift+push = Space */
    if (is_nav(ctl) && nav_keys(link)) {
        static const uint8_t keys[] = {KEY_UP, KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_ENTER};
        if (down) {
            if (ctl == CTL_NAV_PUSH && shift)
                kbd_tap(0, KEY_SPACE);
            else
                kbd_tap(shift ? KMOD_SHIFT : 0, keys[ctl - CTL_NAV_UP]);
        }
        return;
    }

    /* always on the keyboard itself */
    switch (ctl) {
    case CTL_SCALE:
        if (!down) return;
        if (shift) {
            ui_open_menu(UI_MENU_SCALE);
        } else {
            static uint8_t last_mode = SCALE_QUANTIZE;
            if (settings.sp.scale_mode != SCALE_OFF) {
                last_mode = settings.sp.scale_mode;
                settings.sp.scale_mode = SCALE_OFF;
            } else {
                settings.sp.scale_mode = last_mode;
            }
            ui_notify("Scale", settings.sp.scale_mode == SCALE_OFF ? "Off" :
                      settings.sp.scale_mode == SCALE_EASY ? "Easy" : "On");
            app_settings_changed();
        }
        return;
    case CTL_ARP:
        if (!down) return;
        if (shift) {
            ui_open_menu(UI_MENU_ARP);
        } else {
            settings.sp.arp_on = !settings.sp.arp_on;
            ui_notify("Arpeggiator", settings.sp.arp_on ? "On" : "Off");
            app_settings_changed();
        }
        return;
    case CTL_OCT_DOWN:
    case CTL_OCT_UP:
        octave_button(ctl == CTL_OCT_UP, down);
        return;
    case CTL_BROWSER:
        if (down && (shift || link != LINK_HOST)) {
            ui_toggle_menu();
            return;
        }
        break;
    case CTL_TEMPO:
        if (link == LINK_HOST)
            break;
        if (down) {
            if (shift) {
                tap_flash = 0;
                tap_n = 0;
                ui_notify("Tempo Light", "Off");
            } else {
                tap_tempo();
            }
        }
        return;
    }

    switch (link) {
    case LINK_HOST:
        if (ctl == CTL_BROWSER && host_mode_index(VIEW_CLIPS) >= 0) {
            static uint8_t before_clips = VIEW_INSTRUMENT;
            if (down) {
                if (host.view == VIEW_CLIPS) {
                    view_select(host_mode_index(before_clips) >= 0 ? before_clips : target_view[settings.knob_target], 1);
                } else {
                    before_clips = host.view;
                    view_select(VIEW_CLIPS, 1);
                }
            }
            return;
        }
        if (knob_mode_button(ctl, down))
            return;
        host_button(ctl, down);
        return;
    case LINK_MACKIE:
        if (knob_mode_button(ctl, down))
            return;
        /* Logic's own commands through the keyboard interface:
           Quantize = Q (quantize selection), Ideas = Shift+R (capture as recording) */
        if (settings.nav_keys && !shift && (ctl == CTL_QUANTIZE || ctl == CTL_IDEAS)) {
            if (down) {
                kbd_tap(ctl == CTL_IDEAS ? KMOD_SHIFT : 0, ctl == CTL_IDEAS ? KEY_R : KEY_Q);
                ui_notify("Logic", ctl == CTL_IDEAS ? "Capture" : "Quantize");
            }
            return;
        }
        mackie_button(ctl, down);
        return;
    default:
        local_button(ctl, down);
        return;
    }
}

static void encoder(int d)
{
    if (ui_menu_open()) {
        ui_input(UI_TURN, d);
        return;
    }
    int link = app_link();
    if (link == LINK_HOST && host_modal()) {
        if (host.msg_id)
            ui_message_scroll(d);
        else
            host_encoder(d);
        return;
    }
    if (nav_keys(link)) {
        /* turn = next / previous item; Shift + turn = zoom */
        for (int i = 0; i < (d < 0 ? -d : d) && i < 8; i++) {
            if (!shift)
                kbd_tap(0, d > 0 ? KEY_RIGHT : KEY_LEFT);
            else if (link == LINK_HOST)
                kbd_tap(0, d > 0 ? KEY_EQUAL : KEY_MINUS);          /* Live */
            else
                kbd_tap(KMOD_CMD, d > 0 ? KEY_RIGHT : KEY_LEFT);    /* Logic */
        }
        return;
    }
    switch (app_link()) {
    case LINK_HOST: host_encoder(d); break;
    case LINK_MACKIE: mcu_jog(d); break;
    default: ui_home_step(d); break;
    }
}

void app_event(const mk_event_t *ev)
{
    idle_ms = 0;
    if (screen_off) {
        screen_off = 0;
        board_display_power(1);
        ui_invalidate();
    }
    switch (ev->type) {
    case EV_KEY:
        play_key(ev->id, ev->value, ev->value != 0);
        break;
    case EV_BUTTON:
        button(ev->id, ev->value);
        break;
    case EV_ENCODER:
        encoder(ev->value);
        break;
    case EV_KNOB:
        knob_turned(ev->id, ev->value);
        break;
    case EV_KNOB_TOUCH:
        knob_touched(ev->id, ev->value);
        break;
    case EV_STRIP: {
        static uint8_t touched[2];
        play_strip(ev->id, ev->value);
        int t = ev->value >= 0, s = ev->id & 1;
        if (t != touched[s]) {
            touched[s] = t;
            if (app_link() == LINK_HOST)
                host_note(0x48 + s, t);
        }
        break;
    }
    case EV_PEDAL:
        play_pedal(ev->id, ev->value);
        break;
    case EV_AFTERTOUCH:
        play_aftertouch(ev->value);
        break;
    case EV_EXPRESSION:
        play_expression(ev->value);
        break;
    }
}

/* ---------- lights ---------- */

static int tempo_flash_on(int link)
{
    if (settings.tempo_light == TEMPO_LIGHT_OFF)
        return 0;
    int auto_on = settings.tempo_light == TEMPO_LIGHT_ALWAYS || tap_flash;
    /* a DAW beat wins: the light follows the song */
    if (link == LINK_HOST && (host.transport & HOST_TF_PLAYING))
        return now_ms - beat_ms < 70;
    if (link == LINK_MACKIE && now_ms - beat_ms < 1500)
        return now_ms - beat_ms < 70;
    if (settings.sp.ext_clock) {
        if (!sp_clock_alive())
            return 0;
        auto_on = 1;
    }
    if (sp_engine_playing())
        auto_on = 1;
    return auto_on && sp_beat_tick() < 3;
}

static void track_beats(int link)
{
    if (link == LINK_HOST && host.beat_gen != seen_beat_gen) {
        seen_beat_gen = host.beat_gen;
        beat_ms = now_ms;
    }
    /* Mackie: the beats digits of the timecode tick over on every beat */
    if (link == LINK_MACKIE && mcu_led[MCU_LED_BEATS]) {
        if (mcu_timecode[3] != seen_beats[0] || mcu_timecode[4] != seen_beats[1]) {
            seen_beats[0] = mcu_timecode[3];
            seen_beats[1] = mcu_timecode[4];
            beat_ms = now_ms;
        }
    }
}

/* Plug-in lit for instrument / plug-in knobs, Track for mixer knobs,
   M and S available */
static void target_lights(uint8_t *level, uint8_t dim, int clips)
{
    uint8_t t = settings.knob_target;
    int dev = t == KM_INSTRUMENT || t == KM_PLUGIN;
    level[CTL_PLUGIN] = !clips && dev ? 255 : dim;
    level[CTL_TRACK] = !clips && !dev ? 255 : dim;
    level[CTL_MUTE] = level[CTL_SOLO] = dim;
}

static uint8_t host_level(uint8_t state, uint8_t dim, int blink)
{
    switch (state) {
    case 1: return dim;
    case 2: return 255;
    case 3: return blink ? 255 : dim;
    default: return 0;
    }
}

static void update_lights(void)
{
    static uint8_t level[CTL_COUNT];
    int link = app_link(), blink = (now_ms / 250) & 1;
    uint8_t dim = settings.led_dim, on = 255;
    for (int i = 0; i < CTL_COUNT; i++)
        level[i] = 0;

    level[CTL_SHIFT] = shift ? on : dim;
    level[CTL_SCALE] = settings.sp.scale_mode ? on : dim;
    level[CTL_ARP] = settings.sp.arp_on ? on : dim;
    level[CTL_OCT_DOWN] = settings.octave < 0 ? on : dim;
    level[CTL_OCT_UP] = settings.octave > 0 ? on : dim;
    level[CTL_BROWSER] = ui_menu_open() ? on : dim;
    track_beats(link);
    level[CTL_TEMPO] = tempo_flash_on(link) ? on : dim;

    switch (link) {
    case LINK_NONE:
        level[CTL_QUANTIZE] = settings.sp.repeat_on ? on : dim;
        level[CTL_IDEAS] = learning ? (blink ? on : dim) : settings.sp.chord == CHORD_MEMORY ? on : dim;
        level[CTL_PRESET_UP] = level[CTL_PRESET_DOWN] = dim;
        level[CTL_MUTE] = level[CTL_SOLO] = dim;
        if (settings.clock_out && !settings.sp.ext_clock)
            level[CTL_PLAY] = level[CTL_STOP] = dim;
        break;
    case LINK_MACKIE: {
        static const uint8_t mirror[][2] = {
            {CTL_LOOP, MCU_CYCLE}, {CTL_METRO, MCU_CLICK}, {CTL_PLAY, MCU_PLAY},
            {CTL_REC, MCU_RECORD}, {CTL_STOP, MCU_STOP},
        };
        for (unsigned i = 0; i < sizeof mirror / sizeof mirror[0]; i++) {
            uint8_t s = mcu_led[mirror[i][1]];
            level[mirror[i][0]] = s == 2 || (s == 1 && blink) ? on : dim;
        }
        level[CTL_UNDO] = level[CTL_QUANTIZE] = level[CTL_IDEAS] = dim;
        level[CTL_PRESET_UP] = level[CTL_PRESET_DOWN] = dim;
        target_lights(level, dim, 0);
        break;
    }
    case LINK_HOST:
        for (int c = 0; c < CTL_COUNT; c++)
            if (c != CTL_SHIFT && c != CTL_SCALE && c != CTL_ARP && c != CTL_OCT_DOWN &&
                c != CTL_OCT_UP && c != CTL_TEMPO && c != CTL_BROWSER)
                level[c] = host.lost ? (host.light[c] ? dim : 0) : host_level(host.light[c], dim, blink);
        if (host.light[CTL_TEMPO])
            level[CTL_TEMPO] = host_level(host.light[CTL_TEMPO], dim, blink) | level[CTL_TEMPO];
        target_lights(level, dim, host.view == VIEW_CLIPS || host.view >= VIEW_HOST);
        if (host_mode_index(VIEW_CLIPS) >= 0)
            level[CTL_BROWSER] = ui_menu_open() || host.view == VIEW_CLIPS ? on : dim;
        else if (!ui_menu_open())
            level[CTL_BROWSER] = host_level(host.light[CTL_BROWSER], dim, blink);
        break;
    }
    for (int i = 0; i < CTL_COUNT; i++)
        board_light(i, level[i]);
    board_lights_flush();
}

/* ---------- what the host asks for ---------- */

/* Smart Play follows the DAW's key while the host sets one (SCALE) and the
   user lets it (System > DAW Scale) */
static void daw_scale(void)
{
    static int32_t applied = -1;        /* root << 12 | mask, or -1 */
    int on = app_link() == LINK_HOST && host.scale_state && settings.daw_scale == DAW_YES;
    int32_t want = on ? (int32_t)host.scale_root << 12 | host.scale_mask : -1;
    if (want == applied)
        return;
    applied = want;
    sp_set_external_scale(on ? host.scale_root : -1, host.scale_mask, host.scale_name);
    sp_config_changed();
    if (on && settings.sp.scale_mode != SCALE_OFF) {
        static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        char b[24], *p = b;
        for (const char *q = names[sp_scale_root()]; *q; ) *p++ = *q++;
        *p++ = ' ';
        for (const char *q = sp_scale_name(); *q && p < b + sizeof b - 1; ) *p++ = *q++;
        *p = 0;
        ui_notify("Scale from DAW", b);
    }
}

static void host_requests(void)
{
    static uint8_t seen_modes, seen_scale;
    if (host.modes_gen != seen_modes) {    /* MODES: stay if offered, else the first one */
        seen_modes = host.modes_gen;
        uint8_t v = host.view;
        if (host_mode_index(v) < 0) {
            uint8_t views[KNOB_MODE_SLOTS + HOST_MODES];
            int n = host_views(views);
            v = n ? views[0] : host.n_modes ? host.mode_id[0] : v;
        }
        view_select(v, 0);                 /* answers with VIEW */
    }
    if (host.set_mode) {                   /* SET_MODE: advisory */
        uint8_t v = host.set_mode - 1;
        host.set_mode = 0;
        if (settings.daw_view == DAW_YES && host_mode_enabled(v) && v != host.view)
            view_select(v, 1);
        else
            host_set_view(host.view);
    }
    if (host.scale_gen != seen_scale) {
        seen_scale = host.scale_gen;
        daw_scale();
    }
}

/* ---------- main loop ---------- */

void app_init(void)
{
    settings_load();
    keys_route();
    board_display_contrast(settings.contrast);
    play_init();
    mcu_init(daw_send);
    host_init(daw_send, ui_notify);
    ui_init();
    now_ms = last_ms = hal_ms();
}

static void screensaver(uint32_t dt)
{
    uint32_t min = settings_screensaver_min[settings.screensaver];
    if (play_keys_down() || sp_engine_playing())
        idle_ms = 0;
    idle_ms += dt;
    if (!screen_off && min && idle_ms >= min * 60000u) {
        screen_off = 1;
        board_display_power(0);
    }
}

void app_poll(void)
{
    now_ms = hal_ms();
    uint32_t dt = now_ms - last_ms;
    if (!dt)
        return;
    last_ms = now_ms;

    int ticks = sp_tick_us(dt * 1000);
    if (settings.clock_out)
        for (int i = 0; i < ticks; i++) {
            uint8_t b = 0xF8;
            midi_keys(&b, 1);
        }
    play_tick_ms(dt);
#if BOARD_MIDI_DIN
    midi_din_tick_ms(dt);
#endif
#ifdef BOARD_GUIDE_KEYS
    lightguide_tick_ms(dt);
#endif
    ui_tick_ms(dt);
    host_tick_ms(dt);
    settings_tick_ms(dt, !play_keys_down() && !sp_engine_playing());
    screensaver(dt);

    knob_target_sync();
    static int last_link = -1;
    int link = app_link();
    if (link != last_link) {               /* a DAW appeared: put the knobs where they were */
        last_link = link;
        if (link != LINK_NONE)
            knob_target_set(settings.knob_target, 0);
        daw_scale();
    }
    if (link == LINK_HOST)
        host_requests();

    static uint32_t light_acc;
    light_acc += dt;
    if (light_acc >= 10) {
        light_acc = 0;
        update_lights();
    }
}
