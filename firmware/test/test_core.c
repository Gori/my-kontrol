/* Host tests for midi.c, play.c and settings_store.c (make test). */
#include <stdio.h>
#include <string.h>
#include "fake_hal.h"
#include "midi.h"
#include "play.h"
#include "settings.h"
#include "settings_store.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- MIDI in, recorded ---------- */

static uint8_t rx_msg[64][3], rx_port[64];
static int rx_n;
static uint8_t rx_sx[256];
static int rx_sx_n, rx_sx_count;
static uint8_t rx_rt[64];
static int rx_rt_n;

void app_midi_message(uint8_t port, uint8_t st, uint8_t a, uint8_t b)
{
    if (rx_n < 64) {
        rx_msg[rx_n][0] = st; rx_msg[rx_n][1] = a; rx_msg[rx_n][2] = b;
        rx_port[rx_n++] = port;
    }
}

void app_midi_sysex(uint8_t port, const uint8_t *m, uint16_t n)
{
    (void)port;
    memcpy(rx_sx, m, n);
    rx_sx_n = n;
    rx_sx_count++;
}

void app_midi_realtime(uint8_t port, uint8_t b)
{
    (void)port;
    rx_rt[rx_rt_n++] = b;
}

void app_midi_sysex_byte(uint8_t port, uint8_t b) { (void)port; (void)b; }

static void rx_reset(void)
{
    rx_n = rx_sx_n = rx_sx_count = rx_rt_n = 0;
    midi_rx_reset();
}

/* feed bytes as a host would pack them */
static void feed(uint8_t cable, const uint8_t *b, int n)
{
    midi_send(cable, b, n);        /* reuse the packer: also tests it */
    uint8_t p[4];
    while (midi_tx_peek(p)) {
        midi_rx_packet(p);
        midi_tx_pop();
    }
}

static void test_midi_roundtrip(void)
{
    rx_reset();
    uint8_t on[3] = {0x91, 60, 100};
    feed(PORT_KEYS, on, 3);
    CHECK(rx_n == 1 && rx_msg[0][0] == 0x91 && rx_msg[0][1] == 60 && rx_msg[0][2] == 100, "note-on");
    uint8_t pc[2] = {0xC0, 5};
    feed(PORT_DAW, pc, 2);
    CHECK(rx_n == 2 && rx_msg[1][0] == 0xC0 && rx_msg[1][1] == 5 && rx_port[1] == PORT_DAW, "program change on DAW");

    for (int len = 2; len <= 20; len++) {
        uint8_t sx[32];
        sx[0] = 0xF0;
        for (int i = 1; i < len - 1; i++) sx[i] = i;
        sx[len - 1] = 0xF7;
        rx_reset();
        feed(PORT_DAW, sx, len);
        CHECK(rx_sx_count == 1 && rx_sx_n == len && !memcmp(rx_sx, sx, len), "sysex length %d", len);
    }
}

static void test_midi_parser_edges(void)
{
    rx_reset();
    /* running status, realtime inside a message and inside sysex */
    uint8_t p1[4] = {0x09, 0x90, 60, 100};
    uint8_t p2[4] = {0x0F, 0xF8, 0, 0};
    uint8_t p3[4] = {0x09, 62, 90, 0};          /* malformed packing, still bytes */
    midi_rx_packet(p1);
    midi_rx_packet(p2);
    uint8_t rs[4] = {0x0B, 61, 80, 0};          /* CIN B with running-status bytes */
    midi_rx_packet(rs);
    (void)p3;
    CHECK(rx_n == 2 && rx_msg[1][0] == 0x90 && rx_msg[1][1] == 61, "running status (%d msgs)", rx_n);
    CHECK(rx_rt_n == 1 && rx_rt[0] == 0xF8, "realtime");

    /* sysex then data bytes: running status must not resume */
    rx_reset();
    uint8_t a[3] = {0x90, 60, 1};
    feed(PORT_KEYS, a, 3);
    uint8_t sx[4] = {0xF0, 0x7D, 0x01, 0xF7};
    feed(PORT_KEYS, sx, 4);
    uint8_t stray[4] = {0x0F, 61, 0, 0};        /* lone data byte packets */
    midi_rx_packet(stray);
    midi_rx_packet(stray);
    CHECK(rx_n == 1, "no running status after sysex (%d)", rx_n);

    /* overlong sysex is dropped, the next one arrives */
    rx_reset();
    uint8_t big[200];
    big[0] = 0xF0;
    for (int i = 1; i < 199; i++) big[i] = 1;
    big[199] = 0xF7;
    feed(PORT_DAW, big, 200);
    CHECK(rx_sx_count == 0, "overlong sysex dropped");
    feed(PORT_DAW, sx, 4);
    CHECK(rx_sx_count == 1, "sysex after an overlong one");

    /* exactly MIDI_SYSEX_MAX bytes is accepted */
    rx_reset();
    uint8_t max[MIDI_SYSEX_MAX];
    max[0] = 0xF0;
    for (int i = 1; i < MIDI_SYSEX_MAX - 1; i++) max[i] = 2;
    max[MIDI_SYSEX_MAX - 1] = 0xF7;
    feed(PORT_DAW, max, MIDI_SYSEX_MAX);
    CHECK(rx_sx_count == 1 && rx_sx_n == MIDI_SYSEX_MAX, "max-size sysex (%d)", rx_sx_n);
}

static void test_midi_queue_full(void)
{
    uint8_t p[4];
    while (midi_tx_peek(p)) midi_tx_pop();
    uint16_t dropped = midi_tx_dropped;
    int sent = 0;
    for (int i = 0; i < 400; i++)
        sent += midi_send3(PORT_KEYS, 0x90, 60, 100) == 0;
    CHECK(sent == 255 && midi_tx_dropped == dropped + 145, "queue holds 255 packets (%d)", sent);
    uint8_t sx[30] = {0xF0};
    sx[29] = 0xF7;
    CHECK(midi_send(PORT_DAW, sx, 30) != 0, "sysex refused whole when full");
    int n = 0;
    while (midi_tx_peek(p)) { midi_tx_pop(); n++; }
    CHECK(n == 255, "no partial sysex queued (%d)", n);
}

/* ---------- play ---------- */

static int note_balance(int port_ch, int note)
{
    int on = 0;
    uint8_t *m = fake_midi[PORT_KEYS];
    for (int i = 0; i + 2 < fake_midi_n[PORT_KEYS]; ) {
        uint8_t s = m[i];
        if ((s & 0xF0) == 0x90 || (s & 0xF0) == 0x80 || (s & 0xF0) == 0xB0 || (s & 0xF0) == 0xE0) {
            if ((s & 0x0F) == port_ch && m[i + 1] == note) {
                if ((s & 0xF0) == 0x90 && m[i + 2]) on++;
                else if ((s & 0xF0) == 0x80 || (s & 0xF0) == 0x90) on--;
            }
            i += 3;
        } else {
            i++;
        }
    }
    return on;
}

static int all_balanced(void)
{
    fake_drain_midi();
    for (int ch = 0; ch < 16; ch++)
        for (int n = 0; n < 128; n++)
            if (note_balance(ch, n) != 0)
                return 0;
    return 1;
}

static void play_reset(void)
{
    settings_defaults(&settings);
    play_init();
    fake_clear_midi();
}

static void test_play_octave_change(void)
{
    play_reset();
    play_key(10, 100, 1);
    settings.octave = 2;
    play_settings_changed();
    play_key(10, 0, 0);
    CHECK(all_balanced(), "octave change while held");
    settings.transpose = 5;
    play_key(3, 100, 1);
    settings.transpose = -3;
    play_key(3, 0, 0);
    CHECK(all_balanced(), "transpose change while held");
}

static void test_play_channel_change(void)
{
    play_reset();
    play_key(10, 100, 1);
    play_key(12, 100, 1);
    settings.channel = 4;
    play_settings_changed();
    play_key(10, 0, 0);               /* releases after the switch do nothing */
    play_key(12, 0, 0);
    CHECK(all_balanced(), "channel change while held");
    play_key(1, 100, 1);
    fake_drain_midi();
    CHECK(fake_midi[PORT_KEYS][fake_midi_n[PORT_KEYS] - 3] == 0x94, "new notes on channel 5");
    play_key(1, 0, 0);
}

static void test_play_voice_count(void)
{
    play_reset();
    settings.sp.scale_mode = SCALE_QUANTIZE;   /* C# and C both play C */
    play_key(19, 100, 1);                      /* 41 + 19 = 60 */
    play_key(20, 100, 1);                      /* 61 -> 60 */
    play_key(19, 0, 0);
    fake_drain_midi();
    CHECK(note_balance(0, 60) == 1, "shared pitch still sounding (%d)", note_balance(0, 60));
    play_key(20, 0, 0);
    CHECK(all_balanced(), "shared pitch released");
}

static void test_play_split(void)
{
    play_reset();
    settings.split_on = 1;
    settings.split_note = 60;
    settings.lower_channel = 2;
    settings.lower_octave = -1;
    settings.sp.chord = CHORD_TRIAD;           /* Smart Play only on the upper zone */
    play_settings_changed();
    fake_clear_midi();
    play_key(0, 100, 1);                       /* 41 lower: 41 - 12 on ch 3 */
    fake_drain_midi();
    CHECK(fake_midi_n[PORT_KEYS] == 3 && fake_midi[PORT_KEYS][0] == 0x92 && fake_midi[PORT_KEYS][1] == 29,
          "lower zone note %02x %d", fake_midi[PORT_KEYS][0], fake_midi[PORT_KEYS][1]);
    play_key(25, 100, 1);                      /* 66 upper: triad on ch 1 */
    fake_drain_midi();
    CHECK(fake_midi_n[PORT_KEYS] == 12, "upper zone chord (%d bytes)", fake_midi_n[PORT_KEYS]);
    settings.split_on = 0;                     /* split off while both held */
    play_settings_changed();
    play_key(0, 0, 0);
    play_key(25, 0, 0);
    CHECK(all_balanced(), "split toggle while held");
}

static void test_play_panic_pedal(void)
{
    play_reset();
    play_pedal(0, 1);
    play_pedal_unplugged();
    fake_drain_midi();
    uint8_t *m = fake_midi[PORT_KEYS];
    int n = fake_midi_n[PORT_KEYS];
    CHECK(n == 6 && m[3] == 0xB0 && m[4] == 64 && m[5] == 0, "unplug releases sustain");
    play_key(5, 100, 1);
    play_panic();
    play_key(5, 0, 0);
    CHECK(all_balanced(), "panic");
}

/* ---------- settings store ---------- */

static void test_store_roundtrip(void)
{
    fake_reset();
    settings_load();
    CHECK(settings.channel == 0 && settings.sp.bpm_x10 == 1200, "defaults on empty flash");
    for (int i = 0; i < 40; i++) {             /* crosses both pages several times */
        settings.channel = i % 16;
        settings.sp.bpm_x10 = 600 + i;
        CHECK(settings_save_now() == 0, "save %d", i);
    }
    settings_defaults(&settings);
    settings_load();
    CHECK(settings.channel == 39 % 16 && settings.sp.bpm_x10 == 639, "newest record wins (%d %d)",
          settings.channel, settings.sp.bpm_x10);
}

static void test_store_power_cut(void)
{
    fake_reset();
    settings_load();
    settings.channel = 7;
    settings_save_now();
    for (int cut = 0; cut < 60; cut += 3) {
        settings.channel = 7;
        settings_save_now();
        settings.channel = 9;
        fake_flash_fail_after = cut;           /* power fails mid-record */
        settings_save_now();
        fake_flash_fail_after = -1;
        settings_defaults(&settings);
        settings_load();
        CHECK(settings.channel == 7 || settings.channel == 9, "after cut at %d: channel %d", cut, settings.channel);
    }
}

/* the Keys port since the last fake_clear_midi() is exactly these bytes */
static int keys_are(const uint8_t *m, int n)
{
    fake_drain_midi();
    int ok = fake_midi_n[PORT_KEYS] == n && !memcmp(fake_midi[PORT_KEYS], m, n);
    fake_clear_midi();
    return ok;
}
#define KEYS_ARE(...) keys_are((const uint8_t[]){__VA_ARGS__}, sizeof (const uint8_t[]){__VA_ARGS__})

static void test_play_aftertouch(void)
{
    play_reset();
    CHECK(settings.aftertouch == AT_ON, "on by default");
    play_aftertouch(64);
    play_aftertouch(64);
    CHECK(KEYS_ARE(0xD0, 64), "channel pressure, once per value");
    play_aftertouch(0);
    CHECK(KEYS_ARE(0xD0, 0), "back to none");

    settings.split_on = 1;
    play_settings_changed();
    play_aftertouch(10);
    CHECK(KEYS_ARE(0xD0, 10, 0xD1, 10), "both zones, like the pedals");
    settings.split_on = 0;
    play_settings_changed();
    fake_clear_midi();

    play_aftertouch(90);
    fake_clear_midi();
    settings.aftertouch = AT_OFF;
    play_settings_changed();
    CHECK(KEYS_ARE(0xD0, 0), "turned off while pressed: none");
    play_aftertouch(50);
    fake_drain_midi();
    CHECK(fake_midi_n[PORT_KEYS] == 0, "off: nothing sent");

    settings.aftertouch = AT_ON;
    play_settings_changed();
    fake_clear_midi();
    play_aftertouch(70);
    fake_clear_midi();
    settings.channel = 3;
    play_settings_changed();
    CHECK(KEYS_ARE(0xD0, 0), "a channel change ends the pressure on the old channel");
    play_aftertouch(71);
    CHECK(KEYS_ARE(0xD3, 71), "then on the new one");
}

static void test_play_expression(void)
{
    play_reset();
    CHECK(settings.expression_cc == 11, "CC 11 by default");
    play_expression(16383);
    play_expression(16300);                  /* same 7-bit value */
    CHECK(KEYS_ARE(0xB0, 11, 127), "expression CC, once per value");
    play_expression(0);
    CHECK(KEYS_ARE(0xB0, 11, 0), "heel down");
    settings.expression_cc = 4;
    play_expression(8192);
    CHECK(KEYS_ARE(0xB0, 4, 64), "the CC from settings");
    settings.expression_cc = 200;
    settings.aftertouch = 7;
    settings_sanitize(&settings);
    CHECK(settings.expression_cc == 11 && settings.aftertouch == AT_ON, "sanitize");
}

static void test_store_sanitize(void)
{
    fake_reset();
    settings_load();
    settings.knob_page = 9;
    settings.channel = 200;
    settings.sp.scale = 99;
    settings_sanitize(&settings);
    CHECK(settings.knob_page == 0 && settings.channel == 0 && settings.sp.scale == 0, "sanitize");
}

static void test_presets(void)
{
    fake_reset();
    settings_load();
    for (int s = 0; s < 8; s++)
        CHECK(!presets_used(s), "empty slot %d", s);
    settings.channel = 3;
    CHECK(presets_save(2) == 0, "save slot 3");
    settings.channel = 5;
    CHECK(presets_save(6) == 0, "save slot 7");
    settings.channel = 0;
    CHECK(presets_load(2) == 0 && settings.channel == 3, "load slot 3");
    CHECK(presets_load(6) == 0 && settings.channel == 5, "load slot 7");
    CHECK(presets_load(0) != 0, "slot 1 empty");

    /* power cut while saving: the previous block survives */
    for (int cut = 0; cut < 500; cut += 37) {
        settings.channel = 11;
        fake_flash_fail_after = cut;
        presets_save(2);
        fake_flash_fail_after = -1;
        settings.channel = 0;
        CHECK(presets_load(6) == 0 && settings.channel == 5, "slot 7 survives a cut at %d", cut);
        CHECK(presets_load(2) == 0 && (settings.channel == 3 || settings.channel == 11), "slot 3 old or new at %d", cut);
        settings.channel = 3;
        presets_save(2);
    }
}

int main(void)
{
    fake_reset();
#define RUN(t) do { current = #t; t(); } while (0)
    RUN(test_midi_roundtrip);
    RUN(test_midi_parser_edges);
    RUN(test_midi_queue_full);
    RUN(test_play_octave_change);
    RUN(test_play_channel_change);
    RUN(test_play_voice_count);
    RUN(test_play_split);
    RUN(test_play_panic_pedal);
    RUN(test_store_roundtrip);
    RUN(test_store_power_cut);
    RUN(test_play_aftertouch);
    RUN(test_play_expression);
    RUN(test_store_sanitize);
    RUN(test_presets);
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures != 0;
}
