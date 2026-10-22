/* Runs the whole core (app, UI, Smart Play, Mackie, host protocol) on the
   host against scripted input and writes every screen as a PBM image:
   make -C test preview  ->  test/out/NN_name.pbm (and .png with Pillow) */
#include <stdio.h>
#include <string.h>
#include "fake_hal.h"
#include "app.h"
#include "gfx.h"
#include "host.h"
#include "mcu.h"
#include "midi.h"
#include "settings.h"
#include "ui.h"

static int frame;
static const char *outdir;

static void shot(const char *name)
{
    ui_invalidate();
    ui_render();
    char path[256];
    snprintf(path, sizeof path, "%s/%02d_%s.pbm", outdir, frame++, name);
    FILE *f = fopen(path, "w");
    fprintf(f, "P1\n%d %d\n", DISP_W, DISP_H);
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++)
            fputc(gfx_fb[(y >> 3) * DISP_W + x] >> (y & 7) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

static int pinging;
static void host_msg(uint8_t cmd, const char *data, int n);

static void tick(int ms)
{
    for (int i = 0; i < ms; i++) {
        fake_now_ms++;
        app_poll();
        if (pinging && fake_now_ms % 500 == 0)
            host_msg(0x03, "", 0);           /* the script's keep-alive */
    }
    fake_drain_midi();
}

static void ev(int type, int id, int value)
{
    mk_event_t e = {type, id, value};
    app_event(&e);
    tick(5);
}

static void press(int ctl) { ev(EV_BUTTON, ctl, 1); ev(EV_BUTTON, ctl, 0); }
static void turn(int d) { ev(EV_ENCODER, 0, d); }

static void sysex(uint8_t cable, const uint8_t *m, int n)
{
    /* through the real USB-MIDI unpacker */
    uint8_t p[4];
    int i = 0;
    while (n - i > 3) {
        p[0] = cable << 4 | 4; p[1] = m[i]; p[2] = m[i + 1]; p[3] = m[i + 2];
        midi_rx_packet(p);
        i += 3;
    }
    int r = n - i;
    p[0] = cable << 4 | (4 + r); p[1] = m[i]; p[2] = r > 1 ? m[i + 1] : 0; p[3] = r > 2 ? m[i + 2] : 0;
    midi_rx_packet(p);
}

static void host_msg(uint8_t cmd, const char *data, int n)
{
    uint8_t m[64] = {0xF0, 0x7D, 0x4D, 0x4B, cmd};
    memcpy(m + 5, data, n);
    m[5 + n] = 0xF7;
    sysex(PORT_DAW, m, n + 6);
}

static void host_text(uint8_t field, const char *s)
{
    char b[40];
    b[0] = field;
    int n = strlen(s);
    memcpy(b + 1, s, n);
    host_msg(0x10, b, n + 1);
}

static void mcu_lcd_text(int offset, const char *s)
{
    uint8_t m[80] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, offset};
    int n = strlen(s);
    memcpy(m + 7, s, n);
    m[7 + n] = 0xF7;
    sysex(PORT_DAW, m, n + 8);
}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : ".";
    fake_reset();
    app_init();
    app_usb_mounted(1);

    ui_splash();
    {
        char path[256];
        snprintf(path, sizeof path, "%s/%02d_splash.pbm", outdir, frame++);
        FILE *f = fopen(path, "w");
        fprintf(f, "P1\n%d %d\n", DISP_W, DISP_H);
        for (int y = 0; y < DISP_H; y++) {
            for (int x = 0; x < DISP_W; x++)
                fputc(gfx_fb[(y >> 3) * DISP_W + x] >> (y & 7) & 1 ? '1' : '0', f);
            fputc('\n', f);
        }
        fclose(f);
    }
    tick(10);
    shot("home");
    ev(EV_KEY, 19, 96);
    tick(1500);
    shot("home_note");
    ev(EV_KEY, 19, 0);
    press(CTL_SCALE);
    shot("scale_popup");
    tick(1500);
    press(CTL_ARP);
    tick(1500);
    shot("home_scale_arp");
    press(CTL_OCT_UP);
    shot("octave_popup");
    tick(1500);
    ev(EV_KNOB, 2, 5);
    shot("knob_popup");
    tick(1500);
    turn(1);
    shot("home_knobs");
    turn(1);

    /* menus */
    press(CTL_NAV_PUSH);
    shot("menu_main");
    turn(1);
    press(CTL_NAV_PUSH);
    shot("menu_arp");
    turn(7);
    shot("menu_arp_repeat");
    press(CTL_NAV_PUSH);
    shot("menu_arp_repeat_on");
    press(CTL_NAV_LEFT);
    turn(3);
    press(CTL_NAV_PUSH);
    shot("menu_controls");
    press(CTL_NAV_LEFT);
    turn(2);
    press(CTL_NAV_PUSH);
    shot("menu_system");
    turn(6);
    press(CTL_NAV_PUSH);
    shot("confirm_reset");
    press(CTL_NAV_LEFT);
    turn(1);
    press(CTL_NAV_PUSH);
    shot("about");
    press(CTL_NAV_PUSH);
    ev(EV_BUTTON, CTL_SHIFT, 1);
    press(CTL_BROWSER);
    ev(EV_BUTTON, CTL_SHIFT, 0);
    tick(1500);

    /* chord learn with Ideas */
    ev(EV_BUTTON, CTL_IDEAS, 1);
    ev(EV_KEY, 19, 90);
    ev(EV_KEY, 22, 90);
    ev(EV_KEY, 26, 90);
    tick(200);
    shot("learn_hold");
    ev(EV_BUTTON, CTL_IDEAS, 0);
    shot("learned");
    ev(EV_KEY, 19, 0);
    ev(EV_KEY, 22, 0);
    ev(EV_KEY, 26, 0);
    tick(1500);

    /* Mackie */
    uint8_t q[] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xF7};
    sysex(PORT_DAW, q, sizeof q);
    mcu_lcd_text(0, "Cutoff Reso   Attack Decay  SustainRelease Drive  Volume ");
    for (int k = 0; k < 8; k++) {
        uint8_t p[4] = {0x1B, 0xB0, 0x30 + k, (k == 1 ? 0x10 : 0x20) | (1 + k)};
        midi_rx_packet(p);
    }
    uint8_t tc[] = {0x40 + 9, '1' - 0x40 + 0x40, 0};
    (void)tc;
    for (int i = 0; i < 10; i++) {
        uint8_t p[4] = {0x1B, 0xB0, 0x40 + i, "1001 30  2"[9 - i]};
        midi_rx_packet(p);
    }
    mcu_lcd_text(56, "  0.0  -6.2   -12   +1.5  -inf   -3.0   0.0    0.0  ");
    tick(30);
    shot("mackie");
    ev(EV_KNOB, 1, 2);
    shot("mackie_knob");
    tick(1500);
    press(CTL_SOLO);
    shot("mackie_next_mode");
    tick(1500);
    mcu_lcd_text(0, "Audio 1Bass   Keys   Drums  Vox    Gtr    Pad    Master ");
    tick(30);
    shot("mackie_volume");

    /* Live script */
    host_msg(0x00, "\x01", 1);
    pinging = 1;
    host_text(HT_TRACK, "2-Bass");
    host_text(HT_DEVICE, "Operator");
    host_text(HT_SUBTITLE, "Osc 2/6");
    const char *names[8] = {"Coarse", "Fine", "Level", "Attack", "Decay", "Sustain", "Release", "Wave"};
    for (int k = 0; k < 8; k++) {
        host_text(HT_KNOB_NAME + k, names[k]);
        char p[3] = {k, 16 * k + 5, 1};
        host_msg(0x11, p, 3);
    }
    host_text(HT_KNOB_VALUE + 2, "-6.0 dB");
    tick(30);
    shot("live_device");
    ev(EV_KNOB_TOUCH, 2, 1);
    shot("live_knob");
    ev(EV_KNOB_TOUCH, 2, 0);
    tick(1500);
    press(CTL_TRACK);
    tick(1500);
    host_text(HT_SUBTITLE, "Tracks 1-8");
    const char *tracks[8] = {"Drums", "Bass", "Keys", "Vox", "Gtr", "Pad", "FX", "Master"};
    for (int k = 0; k < 8; k++)
        host_text(HT_KNOB_NAME + k, tracks[k]);
    tick(30);
    shot("live_volume");
    press(CTL_SOLO);
    tick(1500);
    shot("live_pan");
    press(CTL_BROWSER);
    tick(1500);
    char g[4 + 32] = {8, 4, 2, 1};
    for (int i = 0; i < 32; i++)
        g[4 + i] = "\1\2\3\1\2\4\5\7"[i % 8];
    host_msg(0x14, g, sizeof g);
    host_text(HT_ROW + 1, "Chorus");
    host_text(HT_COL + 2, "Keys");
    tick(30);
    shot("live_clips");

    printf("%d frames in %s\n", frame, outdir);
    return 0;
}
