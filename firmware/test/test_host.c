/* Host tests for the host protocol, device side (host.c; docs/protocol.md). */
#include <stdio.h>
#include <string.h>
#ifndef __xcore__
#include <stdlib.h>
#endif
#include "fake_hal.h"
#include "host.h"
#include "midi.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* midi.c calls back into the application; nothing to do here */
void app_midi_message(uint8_t port, uint8_t st, uint8_t a, uint8_t b) { (void)port; (void)st; (void)a; (void)b; }
void app_midi_sysex(uint8_t port, const uint8_t *m, uint16_t n) { (void)port; (void)m; (void)n; }
void app_midi_realtime(uint8_t port, uint8_t b) { (void)port; (void)b; }
void app_midi_sysex_byte(uint8_t port, uint8_t b) { (void)port; (void)b; }

/* everything the keyboard sends, as whole messages */
static uint8_t out[64][128];
static int out_len[64], out_n;
static char notified[2][32];

static void capture(const uint8_t *b, uint16_t n)
{
    if (out_n < 64 && n <= 128) {
        memcpy(out[out_n], b, n);
        out_len[out_n++] = n;
    }
}

static void notify(const char *t, const char *v)
{
    snprintf(notified[0], sizeof notified[0], "%s", t);
    snprintf(notified[1], sizeof notified[1], "%s", v);
}

static void reset(void)
{
    fake_reset();
    out_n = 0;
    notified[0][0] = notified[1][0] = 0;
    host_init(capture, notify);
}

/* HOST_CORPUS=file: every message sent to the keyboard is also written there, one per line
   in hex (tools/xcore/xtest.py replays them into the firmware on the board model) */
#ifndef __xcore__
static FILE *corpus;
#endif

static void msg(uint8_t cmd, const uint8_t *d, int n)
{
    uint8_t m[200] = {0xF0, 0x7D, 0x4D, 0x4B, cmd};
    memcpy(m + 5, d, n);
    m[5 + n] = 0xF7;
#ifndef __xcore__
    if (corpus) {
        for (int i = 0; i < n + 6; i++)
            fprintf(corpus, "%02x", m[i]);
        fprintf(corpus, "\n");
    }
#endif
    CHECK(host_sysex(m, n + 6) == 1, "cmd %02x not taken", cmd);
}

/* builds a data buffer: bytes, or strings as "s:" + text (length-prefixed) */
static int put(uint8_t *b, int n, const char *s)
{
    int l = strlen(s);
    b[n++] = l;
    memcpy(b + n, s, l);
    return n + l;
}

/* the last sysex the keyboard sent with this command, or 0 */
static const uint8_t *sent(uint8_t cmd, int *len)
{
    for (int i = out_n - 1; i >= 0; i--)
        if (out_len[i] >= 6 && out[i][0] == 0xF0 && out[i][4] == cmd) {
            *len = out_len[i] - 6;
            return out[i] + 5;
        }
    return 0;
}

static void hello2(void)
{
    uint8_t b[40] = {2, 0, 0x7F};
    int n = put(b, 3, "Any DAW");
    n = put(b, n, "1.0");
    msg(0x00, b, n);
}

static void test_strings(void)
{
    current = "strings";
    char s[32];
    int p = 0;
    /* "Café ♯5 – x" + newline: é = U+00E9, ♯ = U+266F, – = U+2013 */
    uint8_t d[] = {19, 'C', 'a', 'f', 0x7F, 0x00, 0x01, 0x69, ' ', 0x7F, 0x00, 0x4C, 0x6F, '5', ' ',
                   0x7F, 0x00, 0x40, 0x13, 0x0A};
    CHECK(host_string(d, sizeof d, &p, s, sizeof s, 0), "decode failed");
    CHECK(!strcmp(s, "Cafe #5 - "), "got '%s'", s);
    CHECK(p == (int)sizeof d, "pos %d", p);
    p = 0;
    host_string(d, sizeof d, &p, s, sizeof s, 1);
    CHECK(s[strlen(s) - 1] == '\n', "newline kept");

    /* truncation to the destination, and a length past the data */
    uint8_t e[] = {10, 'a', 'b', 'c'};
    p = 0;
    host_string(e, sizeof e, &p, s, 3, 0);
    CHECK(!strcmp(s, "ab"), "got '%s'", s);
    CHECK(p == (int)sizeof e, "pos %d", p);

    /* unknown characters fold to '?', ellipsis to three dots */
    uint8_t f[] = {8, 0x7F, 0x00, 0x7F, 0x7F, 0x7F, 0x00, 0x40, 0x26};
    p = 0;
    host_string(f, sizeof f, &p, s, sizeof s, 0);
    CHECK(!strcmp(s, "?..."), "got '%s'", s);
}

static void test_handshake(void)
{
    current = "handshake";
    reset();
    uint8_t v1 = 1;
    msg(0x00, &v1, 1);
    int len;
    const uint8_t *id = sent(0x01, &len);
    CHECK(id && len == 9 && id[0] == 1, "v1 IDENT len %d", len);
    CHECK(!strcmp(notified[0], "DAW"), "v1 popup '%s'", notified[0]);

    reset();
    hello2();
    id = sent(0x01, &len);
    CHECK(id && id[0] == 2 && len == 20, "v2 IDENT len %d", len);
    if (id && len == 20) {
        CHECK((id[9] << 7 | id[10]) == MIDI_SYSEX_MAX, "max_rx");
        CHECK(id[11] == HOST_LIST_ROWS && id[12] == HOST_LIST_CACHE, "list sizes");
        CHECK((id[13] << 7 | id[14]) == HOST_MSG_CHARS, "msg_chars");
        CHECK(id[15] == HOST_KEYS && id[16] == HOST_KEY_CHARS, "key limits");
    }
    CHECK(host.features == 0x7F, "features %x", host.features);
    CHECK(!strcmp(host.name, "Any DAW") && !strcmp(notified[0], "Any DAW"), "name '%s'", host.name);

    /* silence: lost after 3 s, state kept; HI asks for a new handshake */
    strcpy(host.device, "Kept");
    out_n = 0;
    for (int i = 0; i < 3100; i++)
        host_tick_ms(1);
    CHECK(host.connected && host.lost, "lost");
    CHECK(!strcmp(host.device, "Kept"), "state kept while lost");
    CHECK(sent(0x02, &len), "HI while lost");
    notified[0][0] = 0;
    hello2();
    CHECK(!host.lost && !strcmp(host.device, "Kept"), "re-handshake keeps state");
    CHECK(!notified[0][0], "no second Connected popup");

    /* 30 s lost: gone */
    for (int i = 0; i < 34000; i++)
        host_tick_ms(1);
    CHECK(!host.connected && !host.device[0], "cleared after 30 s lost");

    /* BYE clears at once */
    hello2();
    strcpy(host.device, "X");
    msg(0x04, 0, 0);
    CHECK(!host.connected && !host.device[0], "BYE clears");
}

static void test_pages(void)
{
    current = "pages";
    reset();
    hello2();
    host_set_view(VIEW_DEVICE);
    uint8_t b[80];
    int n;
    /* PAGE for another mode is dropped */
    b[0] = 5; b[1] = VIEW_VOLUME; b[2] = 0; b[3] = 0;
    n = put(b, 4, "Wrong");
    n = put(b, n, "");
    msg(0x20, b, n);
    CHECK(host.page_gen == 0 && strcmp(host.device, "Wrong"), "page for another mode dropped");

    b[0] = 5; b[1] = VIEW_DEVICE; b[2] = 1; b[3] = 3;
    n = put(b, 4, "Reverb");
    n = put(b, n, "Early");
    msg(0x20, b, n);
    CHECK(host.page_gen == 5 && !strcmp(host.device, "Reverb") && host.page_banks == 3, "page");

    /* SLOT: enum, bipolar */
    uint8_t s[40] = {5, 2, SLOT_ENUM, 1, 4, 0x40, 0, 0x7F, 0x7F, 0x7F};
    n = put(s, 10, "Room");
    n = put(s, n, "Shape");
    msg(0x21, s, n);
    CHECK(host.slot_kind[2] == SLOT_ENUM && host.slot_steps[2] == 4, "slot kind");
    CHECK(host.knob_flags[2] == (1 | 2 | 4), "slot flags %x", host.knob_flags[2]);
    CHECK(!strcmp(host.knob_name[2], "Room") && !strcmp(host.group[2], "Shape"), "slot names");

    /* VALUE: text, then position only (text unchanged), then same pos new text */
    uint8_t v[20] = {5, 2, 0x7F, 0x7F};
    n = put(v, 4, "Hall");
    msg(0x22, v, n);
    CHECK(host.knob_pos[2] == 127 && !strcmp(host.knob_value[2], "Hall"), "value");
    v[2] = 0x20; v[3] = 0; v[4] = 0;
    msg(0x22, v, 5);
    CHECK(host.knob_pos[2] == 0x20 && !strcmp(host.knob_value[2], "Hall"), "empty text keeps text");
    n = put(v, 4, "Plate");
    msg(0x22, v, n);
    CHECK(!strcmp(host.knob_value[2], "Plate"), "same pos, new text");

    /* stale gen is dropped */
    v[0] = 4;
    n = put(v, 4, "Stale");
    msg(0x22, v, n);
    CHECK(!strcmp(host.knob_value[2], "Plate"), "stale VALUE dropped");

    /* a new view drops the page */
    host_set_view(VIEW_VOLUME);
    CHECK(host.page_gen == 0 && !host.knob_name[2][0], "view change clears slots");
    v[0] = 5;
    n = put(v, 4, "Late");
    msg(0x22, v, n);
    CHECK(!host.knob_value[2][0], "late VALUE after view change dropped");

    /* meters */
    b[0] = 6; b[1] = VIEW_VOLUME; b[2] = 0; b[3] = 0;
    n = put(b, 4, "");
    n = put(b, n, "Tracks 1-8");
    msg(0x20, b, n);
    uint8_t m[] = {0, 0, 6, 3, 10, 20, 30};
    msg(0x23, m, sizeof m);
    CHECK(host.meter[0] == 10 && host.meter[2] == 30 && host.meter[3] == 0, "slot meters");
    uint8_t c[] = {1, 0, 0, 2, 50, 60};
    msg(0x23, c, sizeof c);
    CHECK(host.channel_meter[0] == 50 && host.channel_meter[1] == 60, "channel meter");
    for (int i = 0; i < 999; i++)
        host_tick_ms(1);
    CHECK(host.meter[0] == 10 && host.channel_meter[0] == 50, "meters held under 1 s");
    msg(0x03, 0, 0);                    /* keep the host alive */
    host_tick_ms(2);
    CHECK(host.meter[0] == 0 && host.channel_meter[0] == 0, "meters expire after 1 s");
}

static void test_modes(void)
{
    current = "modes";
    reset();
    hello2();
    CHECK(host_mode_enabled(VIEW_PAN), "v1 / no MODES: every standard mode");
    uint8_t b[60] = {3, VIEW_VOLUME, 1, 0, VIEW_DEVICE, 0, 0};
    int n = 7;
    b[n++] = 16; b[n++] = 1;
    n = put(b, n, "Takes");
    msg(0x09, b, n);
    CHECK(host.n_modes == 3 && host.modes_gen == 1, "modes parsed");
    CHECK(host_mode_enabled(VIEW_VOLUME) && !host_mode_enabled(VIEW_DEVICE), "enabled flags");
    CHECK(host_mode_index(VIEW_PAN) < 0, "pan not offered");
    CHECK(host_mode_name(16) && !strcmp(host_mode_name(16), "Takes"), "host mode name");
    uint8_t sm = 16;
    msg(0x0A, &sm, 1);
    CHECK(host.set_mode == 17, "SET_MODE recorded");
    host_set_view(16);
    int len;
    const uint8_t *v = sent(0x05, &len);
    CHECK(v && v[0] == 16, "VIEW sent for a host mode");
    msg(0x04, 0, 0);
    CHECK(host.view < VIEW_COUNT, "host mode goes with the host");
}

static void test_keys(void)
{
    current = "keys";
    reset();
    hello2();
    char label[12];
    for (int i = 0; i < HOST_KEYS + 2; i++) {
        uint8_t b[20] = {36 + i, 1, 0, 0, 0};
        snprintf(label, sizeof label, "K%d", i);
        int n = put(b, 5, label);
        msg(0x25, b, n);
    }
    CHECK(host.keys_n == HOST_KEYS, "limit %d", host.keys_n);
    CHECK(host_key_label(36) && !strcmp(host_key_label(36), "K0"), "first kept");
    CHECK(!host_key_label(36 + HOST_KEYS), "over the limit dropped");
    uint8_t r[40] = {36, 2, 0, 0, 0};
    int n = put(r, 5, "Kick 909 long name");
    msg(0x25, r, n);
    CHECK(!strcmp(host_key_label(36), "Kick 909 l"), "replace + truncate: '%s'", host_key_label(36));
    uint8_t c[6] = {37, 0, 0, 0, 0, 0};
    msg(0x25, c, 6);
    CHECK(!host_key_label(37) && host.keys_n == HOST_KEYS - 1, "clear one");
    uint8_t g[20] = {127, 9, 0, 0, 0};
    n = put(g, 5, "G9");
    msg(0x25, g, n);
    CHECK(host_key_label(127), "note 127 is a normal note");
    msg(0x2A, 0, 0);
    CHECK(host.keys_n == 0, "KEYS_CLEAR");
}

static void test_lists(void)
{
    current = "lists";
    reset();
    hello2();
    uint8_t l[40] = {3, 0, 100, 0, 50, 0, 49};
    int n = put(l, 7, "Presets");
    msg(0x26, l, n);
    CHECK(host_modal() && host.list_total == 100 && host.list_cursor == 50, "list");
    for (int i = 44; i < 44 + HOST_LIST_CACHE; i++) {
        uint8_t r[20] = {3, 0, i, 0};
        n = put(r, 4, "row");
        msg(0x27, r, n);
    }
    /* full: 50 is closer than 44 (the farthest), so 44 goes */
    uint8_t r[20] = {3, 0, 50, ROW_FOLDER};
    n = put(r, 4, "Fifty");
    msg(0x27, r, n);
    int has44 = 0, has50 = 0;
    for (int i = 0; i < HOST_LIST_CACHE; i++) {
        has44 |= host.rows[i].index == 44;
        has50 |= host.rows[i].index == 50 && host.rows[i].flags == ROW_FOLDER;
    }
    CHECK(!has44 && has50, "evicts the farthest");
    /* a row farther than every cached one is dropped */
    uint8_t far[20] = {3, 0, 99, 0};
    n = put(far, 4, "Far");
    msg(0x27, far, n);
    int has99 = 0;
    for (int i = 0; i < HOST_LIST_CACHE; i++)
        has99 |= host.rows[i].index == 99;
    CHECK(!has99, "farthest new row dropped");
    /* cache now 45..50; 51 pushes out 45, then 54 ties with 46 (both 4 from
       the cursor) and loses on the higher index */
    uint8_t t51[20] = {3, 0, 51, 0};
    n = put(t51, 4, "51");
    msg(0x27, t51, n);
    uint8_t t54[20] = {3, 0, 54, 0};
    n = put(t54, 4, "54");
    msg(0x27, t54, n);
    int has45 = 0, has46 = 0, has54 = 0;
    for (int i = 0; i < HOST_LIST_CACHE; i++) {
        has45 |= host.rows[i].index == 45;
        has46 |= host.rows[i].index == 46;
        has54 |= host.rows[i].index == 54;
    }
    CHECK(!has45 && has46 && !has54, "tie: the higher index goes");
    /* another list id drops the rows; id 0 closes */
    l[0] = 4;
    n = put(l, 7, "Other");
    msg(0x26, l, n);
    CHECK(host.rows[0].index == HOST_ROW_NONE, "new list drops rows");
    uint8_t z = 0;
    msg(0x26, &z, 1);
    CHECK(!host_modal(), "closed");
}

static void test_message(void)
{
    current = "message";
    reset();
    hello2();
    uint8_t b[80] = {7, 0, MSG_BUSY};
    int n = put(b, 3, "Hello");
    msg(0x28, b, n);
    b[1] = 1; b[2] = MSG_LAST;
    n = put(b, 3, ", world");
    msg(0x28, b, n);
    CHECK(!strcmp(host.msg, "Hello, world") && host.msg_flags == MSG_LAST, "parts: '%s'", host.msg);
    b[1] = 0;
    n = put(b, 3, "New");
    msg(0x28, b, n);
    CHECK(!strcmp(host.msg, "New"), "part 0 replaces");
    for (int i = 0; i < 6; i++) {         /* stops at msg_chars */
        b[1] = 1 + i;
        n = put(b, 3, "0123456789012345678901234567890123456789");
        msg(0x28, b, n);
    }
    CHECK(host.msg_len == HOST_MSG_CHARS, "limit %d", host.msg_len);
    uint8_t z = 0;
    msg(0x28, &z, 1);
    CHECK(!host.msg_id && !host_modal(), "closed");
}

static void test_scale(void)
{
    current = "scale";
    reset();
    hello2();
    uint8_t b[30] = {1, 2, 0x0D, 0x2D};     /* D Dorian: 0,2,3,5,7,9,10 = 0x6AD */
    int n = put(b, 4, "Dorian");
    msg(0x24, b, n);
    CHECK(host.scale_state == 1 && host.scale_root == 2, "scale");
    CHECK(host.scale_mask == 0x6AD, "mask %x", host.scale_mask);
    CHECK(!strcmp(host.scale_name, "Dorian") && host.scale_gen == 1, "name");
}

static void test_describe(void)
{
    current = "describe";
    reset();
    hello2();
    out_n = 0;
    msg(0x07, 0, 0);
    for (int i = 0; i < 20; i++) {
        host_tick_ms(1);
        fake_drain_midi();
    }
    char json[1024];
    int jn = 0, parts = -1, seen = 0;
    for (int i = 0; i < out_n; i++) {
        if (out[i][4] != 0x08)
            continue;
        int part = out[i][5] << 7 | out[i][6];
        parts = out[i][7] << 7 | out[i][8];
        CHECK(part == seen, "part order %d", part);
        seen++;
        memcpy(json + jn, out[i] + 9, out_len[i] - 10);
        jn += out_len[i] - 10;
    }
    json[jn] = 0;
    CHECK(parts == seen && parts > 1, "parts %d of %d", seen, parts);
    CHECK(!strcmp(json, board_surface_json), "reassembled JSON matches");
}

int main(void)
{
#ifndef __xcore__                       /* (also built for xsim, without a file system) */
    const char *cp = getenv("HOST_CORPUS");
    if (cp)
        corpus = fopen(cp, "w");
#endif
    test_strings();
    test_handshake();
    test_pages();
    test_modes();
    test_keys();
    test_lists();
    test_message();
    test_scale();
    test_describe();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
