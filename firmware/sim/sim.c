/* My Kontrol M32 simulator: the real firmware core on macOS, on CoreMIDI
   virtual ports named like the USB device, with the OLED, the button
   lights and the controls in a terminal. See README.md. */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "sim.h"
#include "app.h"
#include "gfx.h"
#include "host.h"
#include "midi.h"
#include "settings.h"
#include "ui.h"

#define SPLASH_MS 1200
#define FRAME_MS 20             /* core frames, as core/main.c */
#define DRAW_MS 33              /* terminal redraws, at most ~30 per second */
#define BUTTON_MS 100           /* a keystroke is a press this long */
#define NOTE_MS 300             /* terminals report no key-up: notes end by themselves */
#define TOUCH_MS 800            /* knob released after this long without turns */
#define ESC_WAIT_MS 50          /* a lone ESC, not the start of an arrow key */

static const char usage[] =
    "usage: mk-sim [--log FILE] [--no-ui]\n"
    "\n"
    "Runs the My Kontrol M32 firmware core with CoreMIDI virtual ports\n"
    "\"My Kontrol Keys\" and \"My Kontrol DAW\" (a source and a destination each).\n"
    "\n"
    "  --log FILE   write every sysex in and out as hex to FILE\n"
    "  --no-ui      headless: no terminal UI, prints only errors; stop with Ctrl-C\n"
    "\n"
    "keys:\n"
    "  knobs 1-8    q w e r t y u i  +1      a s d f g h j k  -1   (hold to repeat)\n"
    "  4D encoder   arrows = up/down/left/right, Enter = push, < > or , . = turn -1/+1\n"
    "  keybed       z x c v b n m /  = keys 0-7, 300 ms notes at velocity 100\n"
    "  Shift        H  (held until H again)\n"
    "  buttons      C Scale  A Arp  U Undo  Q Quantize  I Ideas  L Loop  O Metro\n"
    "               T Tempo  P Play  R Rec  S Stop  [ Preset up  ] Preset down\n"
    "               M Mute  N Solo  B Browser  G Plug-in  K Track  { Octave -  } Octave +\n"
    "  quit         Ctrl-C or Esc Esc\n";

static volatile sig_atomic_t quit_flag, resized;
static int ui_on = 1;

/* ---------- terminal ---------- */

static struct termios saved_tio;
static int raw_on;

static void term_restore(void)
{
    if (!raw_on)
        return;
    raw_on = 0;
    fputs("\033[0m\033[?25h\033[?1049l", stdout);   /* colours, cursor, main screen */
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
}

static int term_raw(void)
{
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_tio))
        return -1;
    struct termios t = saved_tio;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);   /* Ctrl-C arrives as a byte */
    t.c_iflag &= ~(IXON | ICRNL | INLCR);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t))
        return -1;
    raw_on = 1;
    fputs("\033[?1049h\033[?25l\033[2J", stdout);   /* own screen, no cursor */
    fflush(stdout);
    return 0;
}

void sim_quit(int code)
{
    term_restore();
    sim_midi_close();
    if (sim_log)
        fclose(sim_log);
    exit(code);
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH)
        resized = 1;
    else
        quit_flag = 1;
}

/* ---------- controls ---------- */

static void event(int type, int id, int value)
{
    mk_event_t e = {(uint8_t)type, (uint8_t)id, (int16_t)value};
    app_event(&e);
}

static uint32_t button_up[CTL_COUNT];   /* release time, 0 = not pressed */
static uint32_t note_off[8];
static uint32_t knob_seen[BOARD_KNOBS];
static uint8_t knob_touched[BOARD_KNOBS];
static int shift_held;

static void button(int ctl)
{
    if (button_up[ctl])
        event(EV_BUTTON, ctl, 0);
    event(EV_BUTTON, ctl, 1);
    button_up[ctl] = sim_now_ms + BUTTON_MS;
}

static void knob(int k, int d)
{
    if (!knob_touched[k]) {
        knob_touched[k] = 1;
        event(EV_KNOB_TOUCH, k, 1);
    }
    event(EV_KNOB, k, d);
    knob_seen[k] = sim_now_ms;
}

static void note(int k)
{
    if (note_off[k])
        event(EV_KEY, k, 0);
    event(EV_KEY, k, 100);
    note_off[k] = sim_now_ms + NOTE_MS;
}

/* every millisecond: end presses, notes and knob touches */
static void controls_tick(void)
{
    for (int c = 0; c < CTL_COUNT; c++)
        if (button_up[c] && (int32_t)(sim_now_ms - button_up[c]) >= 0) {
            button_up[c] = 0;
            event(EV_BUTTON, c, 0);
        }
    for (int k = 0; k < 8; k++)
        if (note_off[k] && (int32_t)(sim_now_ms - note_off[k]) >= 0) {
            note_off[k] = 0;
            event(EV_KEY, k, 0);
        }
    for (int k = 0; k < BOARD_KNOBS; k++)
        if (knob_touched[k] && sim_now_ms - knob_seen[k] >= TOUCH_MS) {
            knob_touched[k] = 0;
            event(EV_KNOB_TOUCH, k, 0);
        }
}

static const struct { char key; uint8_t ctl; const char *name; } buttons[] = {
    {'H', CTL_SHIFT, "Shift"}, {'C', CTL_SCALE, "Scale"}, {'A', CTL_ARP, "Arp"},
    {'U', CTL_UNDO, "Undo"}, {'Q', CTL_QUANTIZE, "Quantize"}, {'I', CTL_IDEAS, "Ideas"},
    {'L', CTL_LOOP, "Loop"}, {'O', CTL_METRO, "Metro"}, {'T', CTL_TEMPO, "Tempo"},
    {'P', CTL_PLAY, "Play"}, {'R', CTL_REC, "Rec"}, {'S', CTL_STOP, "Stop"},
    {'[', CTL_PRESET_UP, "Preset+"}, {']', CTL_PRESET_DOWN, "Preset-"}, {'M', CTL_MUTE, "Mute"},
    {'N', CTL_SOLO, "Solo"}, {'B', CTL_BROWSER, "Browser"}, {'G', CTL_PLUGIN, "Plug-in"},
    {'K', CTL_TRACK, "Track"}, {'{', CTL_OCT_DOWN, "Octave-"}, {'}', CTL_OCT_UP, "Octave+"},
};
#define N_BUTTONS (int)(sizeof buttons / sizeof buttons[0])

static void key(int c)
{
    static const char up[] = "qwertyui", down[] = "asdfghjk", keys[] = "zxcvbnm/";
    const char *p;
    if (c == 0x03) {
        quit_flag = 1;
    } else if (c == 'H') {
        shift_held = !shift_held;
        event(EV_BUTTON, CTL_SHIFT, shift_held);
    } else if (c && (p = strchr(up, c))) {
        knob(p - up, 1);
    } else if (c && (p = strchr(down, c))) {
        knob(p - down, -1);
    } else if (c && (p = strchr(keys, c))) {
        note(p - keys);
    } else if (c == '<' || c == ',') {
        event(EV_ENCODER, 0, -1);
    } else if (c == '>' || c == '.') {
        event(EV_ENCODER, 0, 1);
    } else if (c == '\r' || c == '\n') {
        button(CTL_NAV_PUSH);
    } else {
        for (int i = 0; i < N_BUTTONS; i++)
            if (buttons[i].key == c)
                button(buttons[i].ctl);
    }
}

/* raw stdin: bytes, CSI arrow keys, Esc Esc */
static uint8_t in_buf[64];
static int in_n;
static uint32_t in_held_ms, esc_ms;
static int esc_seen;

static void lone_esc(void)
{
    if (esc_seen && sim_now_ms - esc_ms < 1000)
        quit_flag = 1;
    esc_seen = 1;
    esc_ms = sim_now_ms;
}

static void input_poll(void)
{
    int had = in_n;
    ssize_t r = read(STDIN_FILENO, in_buf + in_n, sizeof in_buf - in_n);
    if (r > 0)
        in_n += r;
    if (!in_n)
        return;
    if (!had || r > 0)
        in_held_ms = sim_now_ms;
    int wait = sim_now_ms - in_held_ms < ESC_WAIT_MS;   /* the rest of a sequence may follow */
    int i = 0;
    while (i < in_n) {
        uint8_t b = in_buf[i];
        if (b != 0x1B) {
            esc_seen = 0;
            key(b);
            i++;
            continue;
        }
        if (i + 1 >= in_n) {
            if (wait)
                break;
            lone_esc();
            i++;
            continue;
        }
        if (in_buf[i + 1] != '[' && in_buf[i + 1] != 'O') {
            lone_esc();
            i++;
            continue;
        }
        int j = i + 2;
        while (j < in_n && !(in_buf[j] >= 0x40 && in_buf[j] <= 0x7E))
            j++;
        if (j >= in_n) {
            if (wait)
                break;
            i = in_n;                           /* never finished: drop it */
            continue;
        }
        esc_seen = 0;
        if (j == i + 2) {
            switch (in_buf[j]) {
            case 'A': button(CTL_NAV_UP); break;
            case 'B': button(CTL_NAV_DOWN); break;
            case 'C': button(CTL_NAV_RIGHT); break;
            case 'D': button(CTL_NAV_LEFT); break;
            }
        }
        i = j + 1;
    }
    memmove(in_buf, in_buf + i, in_n - i);
    in_n -= i;
}

/* ---------- screen ---------- */

static char frame[32768], shown[32768];
static int fn;

static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frame + fn, sizeof frame - fn, fmt, ap);
    va_end(ap);
    if (n > 0)
        fn = fn + n < (int)sizeof frame ? fn + n : (int)sizeof frame - 1;
}

static void hline(const char *l, const char *r)
{
    put(" %s", l);
    for (int x = 0; x < DISP_W; x++)
        put("─");
    put("%s\033[K\n", r);
}

static int pixel(int x, int y)
{
    return sim_display[y >> 3][x] >> (y & 7) & 1;
}

static const char *level_name(uint8_t v, const char **colour)
{
    if (!v) {
        *colour = "\033[90m";
        return "off";
    }
    if (v < 128) {
        *colour = "\033[33m";
        return "dim";
    }
    *colour = "\033[1;93m";
    return "on";
}

static void draw(void)
{
    fn = 0;
    put("\033[H");
    hline("┌", "┐");
    for (int r = 0; r < DISP_H / 2; r++) {
        put(" │\033[97;40m");
        if (!sim_display_on) {
            const char *t = r == DISP_H / 4 ? "(display off: the screensaver; any control wakes it)" : "";
            int pad = (DISP_W - (int)strlen(t)) / 2;
            put("\033[90m%*s%s%*s", pad, "", t, DISP_W - pad - (int)strlen(t), "");
        } else {
            for (int x = 0; x < DISP_W; x++) {
                int top = pixel(x, 2 * r), bottom = pixel(x, 2 * r + 1);
                put("%s", top && bottom ? "█" : top ? "▀" : bottom ? "▄" : " ");
            }
        }
        put("\033[0m│\033[K\n");
    }
    hline("└", "┘");

    for (int i = 0; i < N_BUTTONS; i++) {
        const char *colour, *lv = level_name(sim_lights[buttons[i].ctl], &colour);
        put("%s %c %-8s %s%-3s\033[0m", i % 7 ? "  " : " ", buttons[i].key, buttons[i].name, colour, lv);
        if (i % 7 == 6 || i == N_BUTTONS - 1)
            put("\033[K\n");
    }
    put("\033[K\n");

    put(" link: ");
    switch (app_link()) {
    case LINK_HOST:
        put("\033[1mhost\033[0m \"%s\" proto %d%s", host.name, host.proto, host.lost ? " \033[1;31mLOST\033[0m" : "");
        break;
    case LINK_MACKIE:
        put("\033[1mMackie\033[0m");
        break;
    default:
        put("none%s", settings.daw_mode == DAW_OFF ? " (DAW Control off)" : "");
    }
    put("    shift: %s", shift_held ? "\033[1;93mheld\033[0m" : "-");
    put("    MIDI packets in %u out %u", sim_midi_in, sim_midi_out);
    if (sim_kbd_last[0] && sim_now_ms - sim_kbd_ms < 2000)
        put("    typed: %s", sim_kbd_last);
    put("\033[K\n\033[K\n");
    put(" \033[2mknobs q..i +1 a..k -1   4D arrows Enter , .   keys z x c v b n m /   "
        "H shift   letters above: buttons   Ctrl-C quit\033[0m\033[K\n\033[J");

    if (resized) {
        resized = 0;
        fputs("\033[2J", stdout);
    } else if (!strcmp(frame, shown)) {
        return;
    }
    memcpy(shown, frame, fn + 1);
    fwrite(frame, 1, fn, stdout);
    fflush(stdout);
}

/* ---------- main loop ---------- */

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000;
}

static void blit_all(void)
{
    for (int p = 0; p < DISP_H / 8; p++)
        board_display_page(p, &gfx_fb[p * DISP_W]);
}

int main(int argc, char **argv)
{
    const char *log_path = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fputs(usage, stdout);
            return 0;
        } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            log_path = argv[++i];
        } else if (!strcmp(argv[i], "--no-ui")) {
            ui_on = 0;
        } else {
            fprintf(stderr, "mk-sim: unknown option %s\n\n%s", argv[i], usage);
            return 2;
        }
    }
    if (log_path && !(sim_log = fopen(log_path, "w"))) {
        fprintf(stderr, "mk-sim: cannot write %s: %s\n", log_path, strerror(errno));
        return 1;
    }
    if (ui_on && !isatty(STDIN_FILENO)) {
        fprintf(stderr, "mk-sim: stdin is not a terminal (use --no-ui)\n");
        return 1;
    }

    sim_hal_init();
    if (sim_midi_open())
        sim_quit(1);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGWINCH, &sa, 0);
    if (ui_on && term_raw()) {
        fprintf(stderr, "mk-sim: cannot put the terminal in raw mode\n");
        sim_quit(1);
    }

    /* the boot order of core/main.c */
    ui_splash();
    blit_all();
    board_display_power(1);
    board_init();
    app_init();
    app_usb_mounted(1);
    uint32_t splash_until = sim_now_ms + SPLASH_MS, last_frame = 0, last_draw = 0;

    uint64_t start = now_us();
    while (!quit_flag) {
        uint32_t real_ms = (now_us() - start) / 1000;
        if (real_ms - sim_now_ms > 200)         /* stopped (Ctrl-Z, sleep): don't replay it */
            sim_now_ms = real_ms - 200;
        if (ui_on)
            input_poll();
        while ((int32_t)(real_ms - sim_now_ms) > 0 && !quit_flag) {
            sim_midi_rx();
            sim_now_ms++;
            controls_tick();
            app_poll();
            sim_midi_tx();
            if ((int32_t)(sim_now_ms - splash_until) >= 0 && sim_now_ms - last_frame >= FRAME_MS &&
                ui_render()) {
                last_frame = sim_now_ms;
                blit_all();
            }
        }
        if (ui_on && sim_now_ms - last_draw >= DRAW_MS) {
            last_draw = sim_now_ms;
            draw();
        }
        struct timespec ts = {0, 500000};
        nanosleep(&ts, 0);
    }
    sim_quit(0);
}
