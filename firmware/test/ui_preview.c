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
#include "play.h"
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
    uint8_t m[160] = {0xF0, 0x7D, 0x4D, 0x4B, cmd};
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

    /* a v2 host: modes, pages with metadata, meters, lists, messages */
    host_msg(0x04, "", 0);
    tick(30);
    host_msg(0x00, "\x02\x00\x3F\x07" "Any DAW" "\x03" "2.1", 15);
    tick(1500);
    host_msg(0x09, "\x07" "\x05\x01\x00" "\x00\x01\x00" "\x01\x01\x00" "\x03\x01\x00" "\x04\x01\x00"
                   "\x10\x01\x05" "Takes" "\x11\x01\x07" "Browser", 34);
    tick(30);
    press(CTL_PLUGIN);
    tick(1500);
    {
        char b[80];
        int n = 0;
        b[n++] = 1; b[n++] = VIEW_INSTRUMENT; b[n++] = 1; b[n++] = 4;
        b[n++] = 6; memcpy(b + n, "Retrox", 6); n += 6;
        b[n++] = 6; memcpy(b + n, "Filter", 6); n += 6;
        host_msg(0x20, b, n);
        const char *pn[8] = {"Cutoff", "Reso", "Drive", "Env", "Attack", "Decay", "Mode", "Key"};
        const char *pv[8] = {"2.4 kHz", "31 %", "+4.0 dB", "50 %", "12 ms", "380 ms", "LP 24", "On"};
        for (int k = 0; k < 8; k++) {
            n = 0;
            b[n++] = 1; b[n++] = k; b[n++] = k == 6 ? 3 : k == 7 ? 2 : 1; b[n++] = k == 3;
            b[n++] = k == 6 ? 4 : 0; b[n++] = 0x40; b[n++] = 0;
            b[n++] = 0; b[n++] = 0; b[n++] = 0;
            int l = strlen(pn[k]);
            b[n++] = l; memcpy(b + n, pn[k], l); n += l;
            b[n++] = 6; memcpy(b + n, "Filter", 6); n += 6;
            host_msg(0x21, b, n);
            n = 0;
            b[n++] = 1; b[n++] = k; b[n++] = 16 * k + 20; b[n++] = 0;
            l = strlen(pv[k]);
            b[n++] = l; memcpy(b + n, pv[k], l); n += l;
            host_msg(0x22, b, n);
        }
        host_msg(0x23, "\x01\x00\x00\x02\x60\x58", 6);
    }
    tick(30);
    shot("v2_instrument");
    ev(EV_KNOB_TOUCH, 0, 1);
    shot("v2_knob_group");
    ev(EV_KNOB_TOUCH, 0, 0);
    tick(1500);
    host_msg(0x2A, "", 0);
    ev(EV_KEY, 0, 100);
    ev(EV_KEY, 0, 0);
    {
        char b[16] = {play_last_note, 1, 0, 0, 0, 4, 'K', 'i', 'c', 'k'};
        host_msg(0x25, b, 10);
    }
    tick(1600);
    ev(EV_KEY, 0, 100);
    shot("v2_key_label");
    ev(EV_KEY, 0, 0);
    tick(1600);
    press(CTL_TRACK);
    tick(1500);
    {
        char b[80];
        int n = 0;
        b[n++] = 2; b[n++] = VIEW_VOLUME; b[n++] = 0; b[n++] = 2;
        b[n++] = 0;
        b[n++] = 10; memcpy(b + n, "Tracks 1-8", 10); n += 10;
        host_msg(0x20, b, n);
        const char *tn[8] = {"Kick", "Snare", "Hats", "Bass", "Keys", "Vox", "Pad", "FX"};
        for (int k = 0; k < 8; k++) {
            n = 0;
            b[n++] = 2; b[n++] = k; b[n++] = 1; b[n++] = 2; b[n++] = 0; b[n++] = 0x64; b[n++] = 0;
            b[n++] = 0x7F; b[n++] = 0x20; b[n++] = 0x10;
            int l = strlen(tn[k]);
            b[n++] = l; memcpy(b + n, tn[k], l); n += l;
            b[n++] = 0;
            host_msg(0x21, b, n);
            n = 0;
            b[n++] = 2; b[n++] = k; b[n++] = 0x60 - 6 * k; b[n++] = 0; b[n++] = 0;
            host_msg(0x22, b, n);
        }
        host_msg(0x23, "\x00\x00\x02\x08\x70\x66\x50\x40\x6A\x30\x00\x10", 12);
    }
    tick(30);
    shot("v2_volume_meters");
    press(CTL_SOLO);
    press(CTL_SOLO);
    press(CTL_SOLO);
    tick(1500);
    {
        host_msg(0x26, "\x01\x00\x05\x00\x01\x00\x00\x0DTakes: Vox 2", 20);
        const char *rows[5] = {"Take 1", "Take 2 (comp)", "Take 3", "Take 4", "Take 5"};
        for (int i = 0; i < 5; i++) {
            char b[40];
            int n = 0;
            b[n++] = 1; b[n++] = 0; b[n++] = i; b[n++] = i == 1 ? 6 : 0;
            int l = strlen(rows[i]);
            b[n++] = l; memcpy(b + n, rows[i], l); n += l;
            host_msg(0x27, b, n);
        }
    }
    tick(30);
    shot("v2_list");
    host_msg(0x26, "\x00", 1);
    {
        const char *t = "Here are three ideas for the chorus: a lift into the relative minor, a half-time drum feel, or doubling the vocal an octave up.";
        int len = strlen(t);
        for (int part = 0; part * 60 < len; part++) {
            char b[80];
            int n = 0, l = len - part * 60 < 60 ? len - part * 60 : 60;
            b[n++] = 1; b[n++] = part; b[n++] = (part + 1) * 60 >= len ? MSG_LAST : MSG_BUSY;
            b[n++] = l; memcpy(b + n, t + part * 60, l); n += l;
            host_msg(0x28, b, n);
        }
    }
    tick(30);
    shot("v2_message");
    turn(2);
    shot("v2_message_scrolled");
    host_msg(0x28, "\x00", 1);
    pinging = 0;
    tick(3200);
    shot("v2_lost");

    printf("%d frames in %s\n", frame, outdir);
    return 0;
}
