/* Host tests for the 5-pin MIDI path through the whole core: the DIN byte
   queue in midi.c and the routing in app.c (make test). Built twice: with
   BOARD_MIDI_DIN 1, and with 0 to check the M32 case stays as it was. */
#include <stdio.h>
#include <string.h>
#include "fake_hal.h"
#include "app.h"
#include "midi.h"
#include "settings.h"
#include "smartplay.h"
#include "ui.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a message from the computer on a USB cable, packed as a host would */
static void usb_in(uint8_t cable, const uint8_t *m, int n)
{
    uint8_t p[4] = {0};
    if (m[0] != 0xF0) {
        p[0] = cable << 4 | (m[0] >= 0xF8 ? 0xF : m[0] == 0xF6 ? 0x5 : m[0] >> 4);
        memcpy(p + 1, m, n);
        midi_rx_packet(p);
        return;
    }
    int i = 0;
    for (; n - i > 3; i += 3) {
        p[0] = cable << 4 | 0x4;
        memcpy(p + 1, m + i, 3);
        midi_rx_packet(p);
    }
    memset(p, 0, sizeof p);
    p[0] = cable << 4 | (0x4 + n - i);
    memcpy(p + 1, m + i, n - i);
    midi_rx_packet(p);
}

static void key(int down)
{
    mk_event_t e = {EV_KEY, 19, down ? 100 : 0};    /* key 19 = note 60 on the host board */
    app_event(&e);
}

static void run_ms(int ms)
{
    for (int i = 0; i < ms; i++) {
        fake_now_ms++;
        app_poll();
    }
}

/* the bytes that went out on a port since the last fake_clear_midi() */
static int sent(int port, const uint8_t *m, int n)
{
    fake_drain_midi();
    return fake_midi_n[port] == n && !memcmp(fake_midi[port], m, n);
}

static void dump(int port)
{
    printf("    port %d:", port);
    for (int i = 0; i < fake_midi_n[port]; i++)
        printf(" %02X", fake_midi[port][i]);
    printf("\n");
}

#define SENT(port, ...) do { \
        static const uint8_t want_[] = {__VA_ARGS__}; \
        int ok_ = sent(port, want_, sizeof want_); \
        CHECK(ok_, "port %d", port); \
        if (!ok_) dump(port); \
    } while (0)
#define NOTHING(port) do { fake_drain_midi(); CHECK(!fake_midi_n[port], "nothing on port %d", port); \
        if (fake_midi_n[port]) dump(port); } while (0)

static void setup(void)
{
    fake_reset();
    fake_now_ms = 5000;
    app_init();
    app_usb_mounted(1);
    app_panic();                                 /* nothing left sounding from the last test */
    midi_rx_reset();
    run_ms(1);
    fake_clear_midi();
}

static void settle(void)
{
    app_settings_changed();
    run_ms(1);
    fake_clear_midi();
}

#if BOARD_MIDI_DIN

static void din_in(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        midi_din_rx_byte(b[i]);
}
#define DIN_IN(...) do { static const uint8_t b_[] = {__VA_ARGS__}; din_in(b_, sizeof b_); } while (0)
#define USB_IN(cable, ...) do { static const uint8_t b_[] = {__VA_ARGS__}; usb_in(cable, b_, sizeof b_); } while (0)
#define DIN_SEND(...) do { static const uint8_t b_[] = {__VA_ARGS__}; midi_send(PORT_DIN, b_, sizeof b_); } while (0)

/* ---------- the byte queue ---------- */

static void test_running_status(void)
{
    setup();
    DIN_SEND(0x90, 60, 100);
    DIN_SEND(0x90, 64, 100);
    DIN_SEND(0x80, 60, 0);
    DIN_SEND(0x80, 64, 0);
    DIN_SEND(0x91, 64, 1);
    SENT(PORT_DIN, 0x90, 60, 100, 64, 100, 0x80, 60, 0, 64, 0, 0x91, 64, 1);
    /* the queue ran dry: the next burst repeats the status */
    fake_clear_midi();
    DIN_SEND(0x91, 65, 1);
    SENT(PORT_DIN, 0x91, 65, 1);
    NOTHING(PORT_KEYS);
}

static void test_realtime_and_system(void)
{
    setup();
    DIN_SEND(0x90, 60, 100);
    DIN_SEND(0xF8);
    DIN_SEND(0x90, 62, 100);
    DIN_SEND(0xF6);
    DIN_SEND(0x90, 64, 100);
    DIN_SEND(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7);
    DIN_SEND(0x90, 65, 100);
    DIN_SEND(0xF2, 0x10, 0x00);
    DIN_SEND(0x90, 67, 100);
    SENT(PORT_DIN, 0x90, 60, 100, 0xF8, 62, 100, 0xF6, 0x90, 64, 100,
         0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7, 0x90, 65, 100, 0xF2, 0x10, 0x00, 0x90, 67, 100);
    uint8_t data[2] = {60, 100};
    CHECK(midi_send(PORT_DIN, data, 2) != 0, "a message must start with a status byte");
}

static void test_queue_full(void)
{
    setup();
    uint16_t dropped = midi_din_dropped, usb_dropped = midi_tx_dropped;
    int ok = 0;
    for (int i = 0; i < 100; i++)
        ok += midi_send3(PORT_DIN, 0x90 | (i & 1), 60, 100) == 0;   /* no running status */
    CHECK(ok == 85 && midi_din_dropped == dropped + 15, "255 bytes = 85 notes (%d)", ok);
    CHECK(midi_tx_dropped == usb_dropped, "USB drops untouched");
    CHECK(midi_send3(PORT_DIN, 0x91, 60, 100) != 0, "full");
    CHECK(midi_send3(PORT_DIN, 0x90, 61, 100) != 0, "a running-status message needs 2 bytes, none left");
    for (int i = 0; i < 30; i++)
        midi_din_tx_byte();
    uint8_t sx[32] = {0xF0};
    sx[29] = sx[31] = 0xF7;
    CHECK(midi_send(PORT_DIN, sx, 32) != 0, "sysex refused whole with 30 bytes free");
    CHECK(midi_send(PORT_DIN, sx, 30) == 0, "sysex of 30 fits");
    int n = 0, last = -1;
    for (int b; (b = midi_din_tx_byte()) >= 0; n++)
        last = b;
    CHECK(n == 255 && last == 0xF7, "queue drains in order (%d bytes, last %02X)", n, last);
}

/* ---------- routing ---------- */

static void test_keys_route(void)
{
    setup();
    CHECK(settings.midi_out == MIDI_OUT_BOTH && !settings.midi_thru, "defaults: USB + MIDI, thru off");
    key(1);
    SENT(PORT_KEYS, 0x90, 60, 100);
    SENT(PORT_DIN, 0x90, 60, 100);
    key(0);
    fake_clear_midi();

    settings.midi_out = MIDI_OUT_USB;
    settle();
    key(1);
    key(0);
    SENT(PORT_KEYS, 0x90, 60, 100, 0x80, 60, 0);
    NOTHING(PORT_DIN);

    settings.midi_out = MIDI_OUT_DIN;
    settle();
    key(1);
    mk_event_t k = {EV_KNOB, 0, 5};              /* knob CC with no DAW */
    app_event(&k);
    NOTHING(PORT_KEYS);
    SENT(PORT_DIN, 0x90, 60, 100, 0xB0, 14, 5);

    /* MIDI Out changed with the note held: it ends on the 5-pin */
    fake_clear_midi();
    settings.midi_out = MIDI_OUT_USB;
    app_settings_changed();
    key(0);
    SENT(PORT_DIN, 0x80, 60, 0, 0xB0, 123, 0, 64, 0);
    fake_clear_midi();
    key(1);
    SENT(PORT_KEYS, 0x90, 60, 100);
    NOTHING(PORT_DIN);
    NOTHING(PORT_EXT);
}

static void test_usb_not_mounted(void)
{
    setup();
    app_usb_mounted(0);
    uint16_t dropped = midi_tx_dropped;
    for (int i = 0; i < 300; i++) {
        key(1);
        key(0);
        fake_drain_midi();                       /* the 5-pin keeps up */
    }
    CHECK(midi_tx_free() == 255 && midi_tx_dropped == dropped, "nothing queued for USB");
    fake_clear_midi();
    DIN_IN(0x92, 40, 50);
    NOTHING(PORT_EXT);
    key(1);
    SENT(PORT_DIN, 0x90, 60, 100);

    /* standalone clock: Play, then 120 BPM for a second */
    settings.clock_out = 1;
    settle();
    mk_event_t play = {EV_BUTTON, CTL_PLAY, 1};
    app_event(&play);
    run_ms(1000);
    fake_drain_midi();
    int clocks = 0;
    for (int i = 0; i < fake_midi_n[PORT_DIN]; i++)
        clocks += fake_midi[PORT_DIN][i] == 0xF8;
    CHECK(fake_midi_n[PORT_DIN] && fake_midi[PORT_DIN][0] == 0xFA, "Start on the 5-pin");
    CHECK(clocks >= 47 && clocks <= 48, "48 clocks a second on the 5-pin (%d)", clocks);
    NOTHING(PORT_KEYS);

    app_usb_mounted(1);
    fake_clear_midi();
    key(0);
    SENT(PORT_KEYS, 0x80, 60, 0);
}

static void test_thru(void)
{
    setup();
    DIN_IN(0x92, 40, 50, 41, 50);                /* running status in */
    SENT(PORT_EXT, 0x92, 40, 50, 0x92, 41, 50);
    NOTHING(PORT_DIN);
    NOTHING(PORT_KEYS);

    settings.midi_thru = 1;
    settle();
    DIN_IN(0x90, 40, 50);
    key(1);
    DIN_IN(40, 0);
    SENT(PORT_DIN, 0x90, 40, 50, 60, 100, 40, 0);    /* merged, one running status */
    SENT(PORT_EXT, 0x90, 40, 50, 0x90, 40, 0);
    SENT(PORT_KEYS, 0x90, 60, 100);

    fake_clear_midi();
    DIN_IN(0xF0, 0x43, 0x10, 0x01, 0xF7, 0xC3, 7, 0xF6);
    SENT(PORT_DIN, 0xF0, 0x43, 0x10, 0x01, 0xF7, 0xC3, 7, 0xF6);
    SENT(PORT_EXT, 0xF0, 0x43, 0x10, 0x01, 0xF7, 0xC3, 7, 0xF6);
}

static void test_ext(void)
{
    setup();
    USB_IN(PORT_EXT, 0x91, 64, 127);
    USB_IN(PORT_EXT, 0x91, 64, 0);
    USB_IN(PORT_EXT, 0xC1, 5);
    USB_IN(PORT_EXT, 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7);
    USB_IN(PORT_EXT, 0xF8);
    SENT(PORT_DIN, 0x91, 64, 127, 64, 0, 0xC1, 5, 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7, 0xF8);
    NOTHING(PORT_KEYS);
    NOTHING(PORT_EXT);

    /* 5-pin in, a sysex longer than one packet, back to the computer */
    fake_clear_midi();
    DIN_IN(0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7);
    SENT(PORT_EXT, 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7);
    NOTHING(PORT_DIN);

    /* a cable the keyboard does not have is the Keys port, as before */
    uint8_t p[4] = {0x3B, 0xB0, 14, 77};
    midi_rx_packet(p);
    CHECK(app_knob_value(0, 0) == 77, "cable 3 = Keys: knob echo (%d)", app_knob_value(0, 0));
    NOTHING(PORT_DIN);
}

static void test_clock_source(void)
{
    setup();
    settings.sp.ext_clock = 1;
    settle();
    uint8_t t0 = sp_beat_tick();
    for (int i = 0; i < 6; i++) {               /* both at once: the first one wins */
        USB_IN(PORT_KEYS, 0xF8);
        DIN_IN(0xF8);
    }
    CHECK(sp_beat_tick() == (t0 + 6) % 24, "follows USB only (%d)", (sp_beat_tick() - t0 + 24) % 24);
    run_ms(1001);
    t0 = sp_beat_tick();
    for (int i = 0; i < 4; i++) {               /* USB went quiet: the 5-pin takes over */
        DIN_IN(0xF8);
        run_ms(1);
        USB_IN(PORT_KEYS, 0xF8);
    }
    CHECK(sp_beat_tick() == (t0 + 4) % 24, "follows the 5-pin only (%d)", (sp_beat_tick() - t0 + 24) % 24);
    t0 = sp_beat_tick();
    run_ms(1001);
    fake_clear_midi();
    USB_IN(PORT_EXT, 0xF8);
    USB_IN(PORT_EXT, 0xF8);
    CHECK(sp_beat_tick() == t0, "EXT clock is for the 5-pin, not the arp");
    SENT(PORT_DIN, 0xF8, 0xF8);
}

/* ---------- sysex of any length, and what may go inside it ---------- */

static uint8_t big[1024];

static void make_big(void)
{
    big[0] = 0xF0;
    for (int i = 1; i < 1023; i++)
        big[i] = i * 7 & 0x7F;
    big[1023] = 0xF7;
}

static int got(int port, const uint8_t *m, int n)
{
    fake_drain_midi();
    return fake_midi_n[port] == n && !memcmp(fake_midi[port], m, n);
}

/* big from the computer on EXT, `times` over; a refused packet waits in USB
   while the 5-pin drains. Returns how often USB had to wait. */
static int ext_big(int times)
{
    int refused = 0;
    uint8_t p[4];
    for (int t = 0; t < times; t++)
        for (int i = 0; i < 1024; ) {
            int r = 1024 - i, k = r > 3 ? 3 : r;
            memset(p, 0, sizeof p);
            p[0] = PORT_EXT << 4 | (r > 3 ? 0x4 : 0x4 + r);
            memcpy(p + 1, big + i, k);
            if (!midi_rx_packet(p)) {
                refused++;
                fake_drain_midi();
                run_ms(1);
                continue;
            }
            i += k;
        }
    return refused;
}

/* let the 5-pin drain for up to a second */
static void drain_ms(int ms)
{
    for (int i = 0; i < ms; i++) {
        fake_drain_midi();
        run_ms(1);
    }
    fake_drain_midi();
}

static void test_long_sysex(void)
{
    setup();
    make_big();
    uint16_t dropped = midi_din_dropped, usb_dropped = midi_tx_dropped;

    /* 5-pin in to the computer, drained as USB would */
    for (int i = 0; i < 1024; i++) {
        midi_din_rx_byte(big[i]);
        if (i % 30 == 0)
            fake_drain_midi();
    }
    CHECK(got(PORT_EXT, big, 1024), "1 KB 5-pin in -> EXT (%d bytes)", fake_midi_n[PORT_EXT]);
    NOTHING(PORT_DIN);

    /* and through, at wire speed: a byte in, a byte out */
    settings.midi_thru = 1;
    settle();
    for (int i = 0; i < 1024; i++) {
        midi_din_rx_byte(big[i]);
        fake_drain_midi();
    }
    CHECK(got(PORT_DIN, big, 1024), "1 KB thru (%d bytes)", fake_midi_n[PORT_DIN]);

    /* the computer to the 5-pin: 1 KB fits the EXT queue, USB never waits */
    settings.midi_thru = 0;
    settle();
    CHECK(ext_big(1) == 0, "USB did not wait");
    drain_ms(1000);
    CHECK(got(PORT_DIN, big, 1024), "1 KB EXT -> 5-pin (%d bytes)", fake_midi_n[PORT_DIN]);

    /* 3 KB does not: USB waits while the EXT queue is full */
    fake_clear_midi();
    int refused = ext_big(3);
    drain_ms(1000);
    CHECK(refused > 0, "USB waited for the EXT queue");
    CHECK(fake_midi_n[PORT_DIN] == 3072 && !memcmp(fake_midi[PORT_DIN], big, 1024) &&
          !memcmp(fake_midi[PORT_DIN] + 2048, big, 1024), "3 KB EXT -> 5-pin (%d bytes)", fake_midi_n[PORT_DIN]);
    CHECK(midi_din_dropped == dropped && midi_tx_dropped == usb_dropped, "nothing dropped");
}

static void test_merge_during_sysex(void)
{
    setup();
    settings.midi_thru = 1;
    settle();
    make_big();
    DIN_IN(0xF0, 1, 2, 3);
    key(1);                                      /* waits for the F7 */
    DIN_IN(0xF8);                                /* realtime goes anywhere */
    USB_IN(PORT_EXT, 0xC1, 5);                   /* waits too */
    CHECK(ext_big(1) == 0, "a 1 KB EXT sysex queues while the 5-pin one streams");
    /* the DAW port meanwhile: HELLO is answered at once */
    USB_IN(PORT_DAW, 0xF0, 0x7D, 0x4D, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x03, 0x53, 0x69, 0x6D, 0x03, 0x31, 0x2E, 0x30, 0xF7);
    fake_drain_midi();
    CHECK(fake_midi_n[PORT_DAW] >= 7 && !memcmp(fake_midi[PORT_DAW], "\xF0\x7D\x4D\x4B\x01\x02", 6),
          "IDENT while the 5-pin is busy (%d bytes)", fake_midi_n[PORT_DAW]);
    DIN_IN(4, 5, 0xF7);
    drain_ms(1000);
    uint8_t *d = fake_midi[PORT_DIN];
    static const uint8_t first[] = {0xF0, 1, 2, 3, 0xF8, 4, 5, 0xF7, 0x90, 60, 100, 0xC1, 5};
    CHECK(fake_midi_n[PORT_DIN] == (int)sizeof first + 1024 && !memcmp(d, first, sizeof first) &&
          !memcmp(d + sizeof first, big, 1024), "thru sysex, the waiting, then the EXT sysex (%d bytes)",
          fake_midi_n[PORT_DIN]);
    SENT(PORT_KEYS, 0x90, 60, 100);
    fake_clear_midi();

    /* too much waiting: the sysex is cut short and the playing goes out */
    uint16_t dropped = midi_din_dropped;
    key(0);
    DIN_IN(0xF0, 1);
    for (int i = 0; i < 9; i++) {
        key(1);
        key(0);
    }
    DIN_IN(2, 3, 0xF7);
    fake_drain_midi();
    uint8_t *m = fake_midi[PORT_DIN];
    int n = fake_midi_n[PORT_DIN];
    CHECK(n == 3 + 3 + 18 * 3 && m[0] == 0x80 && m[3] == 0xF0 && m[4] == 1 && m[5] == 0xF7 && m[6] == 0x90 &&
          m[n - 3] == 0x80, "note-off, F0 01 F7, then 18 notes (%d bytes)", n);
    CHECK(midi_din_dropped == dropped + 1, "the cut is counted");
    fake_clear_midi();

    /* a sysex that stops coming is ended */
    DIN_IN(0xF0, 1, 2);
    key(1);
    fake_drain_midi();
    run_ms(150);
    SENT(PORT_DIN, 0xF0, 1, 2, 0xF7, 0x90, 60, 100);
}

static void test_sysex_queue_full(void)
{
    setup();
    settings.midi_thru = 1;
    settle();
    make_big();
    uint16_t dropped = midi_din_dropped, usb_dropped = midi_tx_dropped;
    din_in(big, 400);                            /* nothing drained */
    din_in(big + 1000, 24);                      /* ... and the end of it */
    fake_drain_midi();
    uint8_t *m = fake_midi[PORT_DIN];
    int n = fake_midi_n[PORT_DIN];
    CHECK(n == 255 && !memcmp(m, big, 254) && m[254] == 0xF7, "5-pin: cut at a full queue, F7 (%d bytes)", n);
    CHECK(midi_din_dropped == dropped + 1, "5-pin cut counted once");
    CHECK(fake_midi_n[PORT_EXT] == 424, "EXT got it all (%d)", fake_midi_n[PORT_EXT]);
    fake_clear_midi();
    DIN_IN(0x92, 40, 50);
    SENT(PORT_DIN, 0x92, 40, 50);

    /* USB full mid-sysex: the rest is dropped, F7 when there is room again */
    settings.midi_thru = 0;
    settle();
    din_in(big, 900);
    fake_drain_midi();
    din_in(big + 900, 124);
    fake_drain_midi();
    m = fake_midi[PORT_EXT];
    n = fake_midi_n[PORT_EXT];
    CHECK(n == 3 * 255 + 1 && !memcmp(m, big, 765) && m[765] == 0xF7, "USB: cut, then F7 (%d bytes)", n);
    CHECK(midi_tx_dropped == usb_dropped + 1, "USB cut counted once");
}

static void test_thru_clock(void)
{
    setup();
    settings.midi_thru = 1;
    settings.clock_out = 1;
    settle();
    DIN_IN(0xF8, 0xFA, 0xFB, 0xFC, 0xFE);
    SENT(PORT_DIN, 0xFE);                        /* our clock, not theirs */
    SENT(PORT_EXT, 0xF8, 0xFA, 0xFB, 0xFC, 0xFE);
    fake_clear_midi();
    settings.sp.ext_clock = 1;                   /* following it: ours is off, theirs goes through */
    settle();
    DIN_IN(0xF8, 0xFA);
    SENT(PORT_DIN, 0xF8, 0xFA);
    fake_clear_midi();
    settings.sp.ext_clock = 0;
    settings.clock_out = 0;
    settle();
    DIN_IN(0xF8, 0xFC);
    SENT(PORT_DIN, 0xF8, 0xFC);
}

/* ---------- S-series inputs and buttons ---------- */

static void test_aftertouch_expression(void)
{
    setup();
    settings.midi_out = MIDI_OUT_DIN;
    settle();
    mk_event_t at = {EV_AFTERTOUCH, 0, 30}, ex = {EV_EXPRESSION, 0, 16383};
    app_event(&at);
    app_event(&ex);
    SENT(PORT_DIN, 0xD0, 30, 0xB0, 11, 127);
    NOTHING(PORT_KEYS);
}

static void test_back_enter(void)
{
    setup();
    mk_event_t e = {EV_BUTTON, CTL_NAV_PUSH, 1};
    app_event(&e);
    e.value = 0;
    app_event(&e);
    CHECK(ui_menu_open(), "menu open");
    mk_event_t enter = {EV_BUTTON, CTL_ENTER, 1}, back = {EV_BUTTON, CTL_BACK, 1};
    app_event(&enter);                           /* into Scale & Chord */
    app_event(&back);
    CHECK(ui_menu_open(), "Back: up one level");
    app_event(&back);
    CHECK(!ui_menu_open(), "Back again: closed");
}

#else   /* the M32: no 5-pin */

static void test_no_din(void)
{
    setup();
    uint8_t on[3] = {0x90, 60, 100};
    CHECK(midi_send(PORT_EXT, on, 3) != 0 && midi_send(PORT_DIN, on, 3) != 0, "no EXT, no DIN");
    CHECK(midi_tx_free() == 255, "nothing queued");
    key(1);
    SENT(PORT_KEYS, 0x90, 60, 100);
    NOTHING(PORT_EXT);
    uint8_t cc[3] = {0xB0, 14, 77};
    usb_in(2, cc, 3);
    CHECK(app_knob_value(0, 0) == 77, "cable 2 = Keys: knob echo (%d)", app_knob_value(0, 0));
    app_usb_mounted(0);
    settings.clock_out = 1;
    settle();
    key(0);
    run_ms(100);
    CHECK(midi_tx_free() == 255, "nothing queued while USB is down");
}

#endif

int main(void)
{
#define RUN(t) do { current = #t; t(); } while (0)
#if BOARD_MIDI_DIN
    RUN(test_running_status);
    RUN(test_realtime_and_system);
    RUN(test_queue_full);
    RUN(test_keys_route);
    RUN(test_usb_not_mounted);
    RUN(test_thru);
    RUN(test_ext);
    RUN(test_clock_source);
    RUN(test_long_sysex);
    RUN(test_merge_during_sysex);
    RUN(test_sysex_queue_full);
    RUN(test_thru_clock);
    RUN(test_aftertouch_expression);
    RUN(test_back_enter);
#else
    RUN(test_no_din);
#endif
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures != 0;
}
