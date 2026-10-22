/* Glue between inputs, Smart Play, the UI and USB MIDI. */
#include "app.h"
#include "hw.h"
#include "display.h"
#include "settings.h"
#include "settings_store.h"
#include "input.h"
#include "knobs.h"
#include "leds.h"
#include "mcu.h"
#include "touch.h"
#include "strips.h"
#include "pedal.h"
#include "smartplay.h"
#include "ui.h"
#include "tusb.h"

#define SYST_RVR REG(0xE000E014)
#define SYST_CVR REG(0xE000E018)

enum { CABLE_KEYS, CABLE_DAW };

uint8_t hw_status;   /* app.h HW_* bits */

static void debug_event(uint8_t kind, uint8_t a, uint8_t b);

static void midi_send3(uint8_t status, uint8_t a, uint8_t b)
{
    uint8_t msg[3] = {status, a, b};
    if (tud_midi_mounted())
        tud_midi_stream_write(CABLE_KEYS, msg, 3);
}

static void mcu_send(const uint8_t *b, uint16_t n)
{
    if (tud_midi_mounted())
        tud_midi_stream_write(CABLE_DAW, b, n);
}

/* Byte-stream MIDI parser per cable: running status, sysex up to 128 bytes. */
typedef struct {
    uint8_t status, n, need;
    uint8_t data[2];
    uint8_t sx[128];
    uint8_t sx_len, in_sx;
} parser_t;

static parser_t parsers[2];

static void on_realtime(uint8_t b)
{
    switch (b) {
    case 0xF8: sp_midi_clock(); break;
    case 0xFA: case 0xFB: sp_midi_start(); break;
    case 0xFC: sp_midi_stop(); break;
    }
}

static void on_message(uint8_t cable, uint8_t st, const uint8_t *d)
{
    if (cable != CABLE_DAW)
        return;
    switch (st & 0xF0) {
    case 0x90: mcu_note(d[0], d[1]); break;
    case 0x80: mcu_note(d[0], 0); break;
    case 0xB0: mcu_cc(d[0], d[1]); break;
    }
}

static void parse_byte(uint8_t cable, uint8_t b)
{
    parser_t *p = &parsers[cable];
    if (b >= 0xF8) {
        on_realtime(b);
        return;
    }
    if (b == 0xF0) {
        p->in_sx = 1;
        p->sx_len = 0;
        p->sx[p->sx_len++] = b;
        return;
    }
    if (p->in_sx) {
        if (p->sx_len < sizeof p->sx)
            p->sx[p->sx_len++] = b;
        if (b == 0xF7) {
            p->in_sx = 0;
            if (cable == CABLE_DAW && p->sx_len < sizeof p->sx)
                mcu_sysex(p->sx, p->sx_len);
        }
        if (!(b & 0x80) || b == 0xF7)
            return;
        p->in_sx = 0;                  /* status byte aborts sysex */
    }
    if (b & 0x80) {
        p->status = b;
        p->n = 0;
        uint8_t hi = b & 0xF0;
        p->need = (hi == 0xC0 || hi == 0xD0 || b == 0xF3 || b == 0xF1) ? 1 : (b >= 0xF4 ? 0 : 2);
        return;
    }
    if (!p->status)
        return;
    p->data[p->n++] = b;
    if (p->n >= p->need) {
        on_message(cable, p->status, p->data);
        p->n = 0;
        if (p->status >= 0xF0)
            p->status = 0;             /* no running status for system common */
    }
}

/* Mackie LCD rows are 8 strips of 7 chars; the OLED shows 4 strips per line
   (6 chars each) using the names row only. */
static void update_daw_text(void)
{
    static uint16_t gen = 0xFFFF;
    if (gen == mcu_lcd_gen)
        return;
    gen = mcu_lcd_gen;
    char top[25], bottom[25];
    for (int s = 0; s < 4; s++)
        for (int c = 0; c < 6; c++) {
            top[s * 6 + c] = mcu_lcd[0][s * 7 + c];
            bottom[s * 6 + c] = mcu_lcd[0][(s + 4) * 7 + c];
        }
    top[24] = bottom[24] = 0;
    ui_set_daw_text(top, bottom);
}

static void sp_out(uint8_t note, uint8_t vel, int on)
{
    midi_send3((on ? 0x90 : 0x80) | settings.channel, note, on ? vel : 0);
}

void app_settings_changed(void)
{
    sp_config_changed();
    display_contrast(settings.contrast);
    settings_touch();
}

void app_key(uint8_t key, uint8_t velocity, int on)
{
    int n = key + 12 * settings.octave + settings.transpose;
    if (n < 0 || n > 127)
        return;
    if (on) {
        switch (settings.vel_curve) {
        case VEL_SOFT: velocity = 127 - (127 - velocity) * (127 - velocity) / 127; break;
        case VEL_HARD: velocity = velocity * velocity / 127; break;
        case VEL_FIXED: velocity = settings.fixed_velocity; break;
        default: break;
        }
        if (!velocity)
            velocity = 1;
    }
    sp_key(n, velocity, on);
}

static void midi_in(void)
{
    uint8_t buf[48], cable;
    uint32_t n;
    while (tud_midi_available() && (n = tud_midi_demux_stream_read(&cable, buf, sizeof buf)) > 0)
        for (uint32_t i = 0; i < n; i++)
            parse_byte(cable < 2 ? cable : CABLE_KEYS, buf[i]);
}

void app_init(void)
{
    settings_load();
    display_contrast(settings.contrast);
    sp_init(&settings.sp, sp_out);
    mcu_init(mcu_send);
    ui_init();
    knobs_init();
    if (leds_init() == 0) hw_status |= HW_LEDS;
    if (touch_init() == 0) hw_status |= HW_TOUCH;
    hw_status |= strips_init() << 2;
    leds_all(LED_DIM);
    pedal_init();
    input_init();
}

/* ---------- knobs ---------- */

static uint8_t knob_val[4][8];    /* per page */
static uint8_t touch_mask;

static void show_knob(int k)
{
    char t[16] = "P1 Knob1 CC", v[8];
    t[1] = '1' + settings.knob_page;
    t[7] = '1' + k;
    int cc = settings.knob_cc[settings.knob_page][k];
    t[11] = '0' + cc / 100 % 10; t[12] = '0' + cc / 10 % 10; t[13] = '0' + cc % 10; t[14] = 0;
    if (cc < 100) { t[11] = t[12]; t[12] = t[13]; t[13] = 0; }
    if (cc < 10) { t[11] = t[12]; t[12] = 0; }
    int x = knob_val[settings.knob_page][k];
    v[0] = '0' + x / 100; v[1] = '0' + x / 10 % 10; v[2] = '0' + x % 10; v[3] = 0;
    ui_notify(t, x >= 100 ? v : x >= 10 ? v + 1 : v + 2);
}

static void knob_moved(uint8_t k, int8_t d)
{
    if (settings.daw_mode) {
        mcu_vpot(k, d);
        return;
    }
    int x = knob_val[settings.knob_page][k] + d;
    x = x < 0 ? 0 : x > 127 ? 127 : x;
    if (x == knob_val[settings.knob_page][k])
        return;
    knob_val[settings.knob_page][k] = x;
    midi_send3(0xB0 | settings.channel, settings.knob_cc[settings.knob_page][k], x);
    show_knob(k);
}

static void knob_touched(uint8_t mask)
{
    uint8_t pressed = mask & ~touch_mask;
    debug_event(3, mask, mask >> 7);
    for (int k = 0; k < 8; k++) {
        if (settings.daw_mode && ((mask ^ touch_mask) >> k & 1))
            mcu_vpot_touch(k, mask >> k & 1);
        else if (pressed >> k & 1)
            show_knob(k);
    }
    touch_mask = mask;
}

/* ---------- touch strips ---------- */

/* Both strips run on a 0..16383 scale internally: pitch bend goes out at full
   14-bit resolution, mod as its top 7 bits. Positions 1..200; the outer 12 at
   each end are dead zones so the extremes are reachable (measured: fingers
   land at ~12..188). */
#define STRIP_LO 12
#define STRIP_HI 188

static int32_t strip_out[2] = {8192, 0}, strip_target[2] = {8192, 0};
static uint8_t mod_sent = 0xFF;

static void poll_strips(void)
{
    static uint8_t which;
    uint8_t pos;
    which ^= 1;
    if (!(hw_status & (HW_PITCH << which)) || strips_read(which, &pos))
        return;
    int32_t v = -1;
    if (pos >= 3) {
        int p = pos < STRIP_LO ? STRIP_LO : pos > STRIP_HI ? STRIP_HI : pos;
        v = (p - STRIP_LO) * 16383 / (STRIP_HI - STRIP_LO);
    }
    if (which == STRIP_PITCH)
        strip_target[0] = v < 0 ? 8192 : v;          /* untouched: centre */
    else if (v >= 0)
        strip_target[1] = v;                         /* mod holds when released */

    /* slew like the stock firmware: small moves directly, big jumps in steps */
    int32_t *o = &strip_out[which], diff = strip_target[which] - *o;
    if (!diff)
        return;
    if (diff > 2560) diff = 640;
    else if (diff < -2560) diff = -640;
    *o += diff;
    if (which == STRIP_PITCH) {
        midi_send3(0xE0 | settings.channel, *o & 0x7F, *o >> 7);
    } else if ((*o >> 7) != mod_sent) {
        mod_sent = *o >> 7;
        midi_send3(0xB0 | settings.channel, settings.strip_mod_cc, mod_sent);
    }
}

/* ---------- LEDs ---------- */

/* Buttons whose light mirrors a Mackie LED from the DAW (index = button = LED). */
static const uint8_t mcu_led_for[][2] = {
    {0x06, MCU_CYCLE}, {0x07, MCU_CLICK}, {0x09, MCU_PLAY}, {0x0A, MCU_RECORD},
    {0x0B, MCU_STOP}, {0x0E, MCU_MUTE}, {0x0F, MCU_SOLO}, {0x11, MCU_ASSIGN_PLUGIN},
    {0x12, MCU_ASSIGN_PAN},
};

static void update_leds(void)
{
    extern uint8_t shift_held_for_leds;
    uint32_t now = input_ms;
    int blink = (now / 250) & 1;

    for (int i = 0; i < LED_COUNT; i++)
        leds_set(i, LED_DIM);
    leds_set(0x00, shift_held_for_leds ? LED_ON : LED_DIM);
    leds_set(0x01, settings.sp.scale_mode ? LED_ON : LED_DIM);
    leds_set(0x02, settings.sp.arp_on ? LED_ON : LED_DIM);
    leds_set(0x13, settings.octave < 0 ? LED_ON : LED_DIM);
    leds_set(0x14, settings.octave > 0 ? LED_ON : LED_DIM);

    /* Tempo flashes on each beat of the internal clock */
    uint32_t beat_ms = 600000u / (settings.sp.bpm_x10 ? settings.sp.bpm_x10 : 1200);
    leds_set(0x08, now % beat_ms < 60 ? LED_ON : LED_DIM);

    if (mcu_connected)
        for (unsigned i = 0; i < sizeof mcu_led_for / sizeof mcu_led_for[0]; i++) {
            uint8_t s = mcu_led[mcu_led_for[i][1]];
            leds_set(mcu_led_for[i][0], s == 2 || (s == 1 && blink) ? LED_ON : LED_DIM);
        }
    leds_flush();
}

/* Panel buttons. Identities from re/hardware_map.md 2; the rest show their
   index until they are mapped on the hardware. */
enum {
    BTN_SHIFT = 0x00, BTN_SCALE = 0x01, BTN_ARP = 0x02, BTN_UNDO = 0x03, BTN_QUANTIZE = 0x04,
    BTN_IDEAS = 0x05, BTN_LOOP = 0x06, BTN_METRO = 0x07, BTN_TEMPO = 0x08, BTN_PLAY = 0x09,
    BTN_REC = 0x0A, BTN_STOP = 0x0B, BTN_PRESET_UP = 0x0C, BTN_PRESET_DOWN = 0x0D,
    BTN_MUTE = 0x0E, BTN_SOLO = 0x0F,
    BTN_BROWSER = 0x10, BTN_PLUGIN = 0x11, BTN_TRACK = 0x12,   /* order to be confirmed */
    BTN_OCT_DOWN = 0x13, BTN_OCT_UP = 0x14,
    BTN_UP = 0x15, BTN_LEFT = 0x16, BTN_RIGHT = 0x17, BTN_DOWN = 0x18, BTN_ENC_PUSH = 0x19,
    BTN_PEDAL_PLUG = 0x1A,
};

static uint8_t shift_held;
uint8_t shift_held_for_leds;

static void show_octave(void)
{
    char v[4] = {settings.octave < 0 ? '-' : '+', '0' + (settings.octave < 0 ? -settings.octave : settings.octave), 0};
    ui_notify("Octave", v);
}

/* Hardware-mapping aid: every raw control event as F0 7D <kind> <a> <b> F7 on
   the DAW port (0x7D = non-commercial ID). kind 1 button, 2 encoder, 3 touch. */
static void debug_event(uint8_t kind, uint8_t a, uint8_t b)
{
#ifdef HW_DEBUG
    uint8_t m[6] = {0xF0, 0x7D, kind, a & 0x7F, b & 0x7F, 0xF7};
    mcu_send(m, 6);
#else
    (void)kind; (void)a; (void)b;
#endif
}

/* Buttons that are plain Mackie Control keys, with their Shift variants. */
static const struct { uint8_t btn, mcu, mcu_shift; } mcu_keys[] = {
    {BTN_UNDO,     MCU_UNDO,   MCU_UNDO},     /* Shift+Undo = redo: sent with MCU Shift held */
    {BTN_QUANTIZE, MCU_F1,     MCU_F1 + 4},
    {BTN_IDEAS,    MCU_F1 + 1, MCU_F1 + 5},
    {BTN_LOOP,     MCU_CYCLE,  MCU_CYCLE},
    {BTN_METRO,    MCU_CLICK,  MCU_CLICK},
    {BTN_PLAY,     MCU_PLAY,   MCU_REWIND},   /* Shift+Play: back to start */
    {BTN_REC,      MCU_RECORD, MCU_RECORD},
    {BTN_STOP,     MCU_STOP,   MCU_STOP},
    {BTN_PRESET_UP,   MCU_UP,   MCU_BANK_LEFT},
    {BTN_PRESET_DOWN, MCU_DOWN, MCU_BANK_RIGHT},
    {BTN_MUTE,     MCU_MUTE,   MCU_MUTE},     /* strip 1 of the current bank */
    {BTN_SOLO,     MCU_SOLO,   MCU_SOLO},
    {BTN_PLUGIN,   MCU_ASSIGN_PLUGIN, MCU_ASSIGN_INSTR},
    {BTN_TRACK,    MCU_ASSIGN_PAN,    MCU_ASSIGN_SEND},
};

static uint32_t taps[3], tap_n;

static void tap_tempo(void)
{
    uint32_t now = input_ms;
    if (tap_n && now - taps[(tap_n - 1) % 3] > 2000)
        tap_n = 0;                         /* a pause starts a new measurement */
    taps[tap_n++ % 3] = now;
    if (tap_n < 2)
        return;
    int n = tap_n < 3 ? tap_n : 3;
    uint32_t first = taps[(tap_n - n) % 3], span = now - first;
    uint32_t interval = span / (n - 1);
    if (interval < 200 || interval > 2000)
        return;
    settings.sp.bpm_x10 = (600000 + interval / 2) / interval;
    char v[8];
    int b = settings.sp.bpm_x10;
    v[0] = b >= 1000 ? '0' + b / 1000 : ' '; v[1] = '0' + b / 100 % 10; v[2] = '0' + b / 10 % 10;
    v[3] = '.'; v[4] = '0' + b % 10; v[5] = 0;
    ui_notify("Tempo", v[0] == ' ' ? v + 1 : v);
    app_settings_changed();
}

static void app_button(uint8_t idx, int down)
{
    debug_event(1, idx, down);
    if (idx == BTN_SHIFT) {
        shift_held = shift_held_for_leds = down;
        return;
    }
    /* MIDI mode: Preset up/down select the knob page */
    if (!settings.daw_mode && (idx == BTN_PRESET_UP || idx == BTN_PRESET_DOWN)) {
        if (down) {
            settings.knob_page = (settings.knob_page + (idx == BTN_PRESET_UP ? 1 : 3)) & 3;
            char v[4] = {'0' + settings.knob_page + 1, 0};
            ui_notify("Knob Page", v);
            app_settings_changed();
        }
        return;
    }
    for (unsigned i = 0; i < sizeof mcu_keys / sizeof mcu_keys[0]; i++) {
        if (mcu_keys[i].btn != idx)
            continue;
        uint8_t note = shift_held ? mcu_keys[i].mcu_shift : mcu_keys[i].mcu;
        if (idx == BTN_UNDO && shift_held) {
            mcu_button(MCU_SHIFT, down);
            mcu_button(note, down);
            if (!down) mcu_button(MCU_SHIFT, 0);
        } else {
            mcu_button(note, down);
        }
        return;
    }
    if (!down)
        return;
    switch (idx) {
    case BTN_SCALE:
        if (shift_held) {
            ui_open_menu(UI_MENU_SCALE);
        } else {
            settings.sp.scale_mode = settings.sp.scale_mode ? SCALE_OFF : SCALE_QUANTIZE;
            ui_notify("Scale", settings.sp.scale_mode ? "On" : "Off");
            app_settings_changed();
        }
        return;
    case BTN_ARP:
        if (shift_held) {
            ui_open_menu(UI_MENU_ARP);
        } else {
            settings.sp.arp_on = !settings.sp.arp_on;
            ui_notify("Arpeggiator", settings.sp.arp_on ? "On" : "Off");
            app_settings_changed();
        }
        return;
    case BTN_TEMPO:
        tap_tempo();
        return;
    case BTN_BROWSER:
        if (ui_menu_open())
            ui_event(EV_BACK, 0, 0);
        else
            ui_event(EV_SHOW_MENU, 0, 0);
        return;
    case BTN_UP: case BTN_DOWN: case BTN_LEFT: case BTN_RIGHT: case BTN_ENC_PUSH:
        if (ui_menu_open()) {
            static const uint8_t ev[] = {EV_ENC_UP, EV_ENC_DOWN, EV_ENC_LEFT, EV_ENC_RIGHT, EV_ENC_PUSH};
            ui_event(ev[idx == BTN_UP ? 0 : idx == BTN_DOWN ? 1 : idx == BTN_LEFT ? 2 : idx == BTN_RIGHT ? 3 : 4], 0, 0);
        } else if (settings.daw_mode) {
            static const uint8_t m[] = {MCU_UP, MCU_DOWN, MCU_LEFT, MCU_RIGHT, MCU_ENTER};
            uint8_t n = m[idx == BTN_UP ? 0 : idx == BTN_DOWN ? 1 : idx == BTN_LEFT ? 2 : idx == BTN_RIGHT ? 3 : 4];
            mcu_button(n, 1);
            mcu_button(n, 0);
        } else if (idx == BTN_LEFT || idx == BTN_RIGHT) {
            int t = settings.transpose + (idx == BTN_RIGHT ? 1 : -1);
            if (t >= -12 && t <= 12) {
                settings.transpose = t;
                char v[4] = {t < 0 ? '-' : '+', '0' + (t < 0 ? -t : t) / 10, '0' + (t < 0 ? -t : t) % 10, 0};
                ui_notify("Transpose", v[1] == '0' ? (v[1] = v[0], v + 1) : v);
                app_settings_changed();
            }
        } else if (idx == BTN_ENC_PUSH) {
            ui_event(EV_SHOW_MENU, 0, 0);
        }
        return;
    case BTN_OCT_DOWN:
        if (shift_held) {
            settings.vel_curve = settings.vel_curve == VEL_FIXED ? VEL_LINEAR : VEL_FIXED;
            ui_notify("Fixed Velocity", settings.vel_curve == VEL_FIXED ? "On" : "Off");
            app_settings_changed();
        } else if (settings.octave > -3) {
            settings.octave--;
            show_octave();
            app_settings_changed();
        }
        return;
    case BTN_OCT_UP:
        if (settings.octave < 3) {
            settings.octave++;
            show_octave();
            app_settings_changed();
        }
        return;
    }
}

static void poll_inputs(void)
{
    input_ev_t ev;
    while (input_next(&ev)) {
        switch (ev.type) {
        case IN_KEY:
            app_key(41 + ev.index, ev.value, ev.value != 0);   /* key 0 = F2 */
            break;
        case IN_BUTTON:
            app_button(ev.index, ev.value);
            break;
        case IN_ENCODER: {
            /* 2 quadrature edges per detent (measured) */
            static int8_t acc;
            debug_event(2, ev.delta < 0, ev.delta < 0 ? -ev.delta : ev.delta);
            acc += ev.delta;
            int steps = acc / 2;
            if (steps) {
                acc -= steps * 2;
                if (ui_menu_open())
                    ui_event(EV_ENC_TURN, 0, steps);
                else if (settings.daw_mode)
                    mcu_jog(steps);
                else
                    ui_event(EV_SHOW_MENU, 0, 0);
            }
            break;
        }
        }
    }
}

void app_poll(void)
{
    static uint32_t last_ms;
    midi_in();
    poll_inputs();
    if (settings.daw_mode)
        update_daw_text();
    uint8_t k;
    int8_t d;
    if (knobs_poll(&k, &d))
        knob_moved(k, d);
    uint32_t now = input_ms, dt = now - last_ms;
    if (dt) {
        last_ms = now;
        sp_tick_us(dt * 1000);
        ui_tick_ms(dt);
        settings_tick_ms(dt);
        static uint8_t div, sdiv;
        if (++sdiv >= 5) {             /* strips: each one every 10 ms */
            sdiv = 0;
            poll_strips();
        }
        if (++div >= 10) {             /* 10 ms housekeeping */
            div = 0;
            uint8_t m, sw, down;
            if (touch_poll(&m))
                knob_touched(m);
            if (pedal_poll(&sw, &down))
                midi_send3(0xB0 | settings.channel, settings.pedal_cc[sw], down ? 127 : 0);
            update_leds();
        }
    }
    ui_render();
}
