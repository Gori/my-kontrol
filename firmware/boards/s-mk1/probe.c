/* S MK1 bring-up probe (probe.h), in hybrid developer builds (MK_PROBE): our tile 0 next to
   NI's tile 1 (platform/xs1/ni_link.c), so the probe's MIDI goes out through NI's USB-MIDI
   port (and its 5-pin OUT, which gets the same packets).

   Out (cable 0, MIDI channel 16, at most one packet per millisecond, as NI's tile 1 takes
   them): control changes, one per byte, because NI's tile 1 passes those on whole (it cut
   SysEx short on the emulator's 5-pin OUT): CC 15 = <type> starts a message, then each
   payload byte b as CC (16 + (b >> 7)) = b & 0x7F, then CC 14 = the sum of the payload
   bytes & 0x7F ends it. Types:
     01  the 32 input bits (4 bytes, bit 0 first), when they change
     02  sensor 0x12's reply (22 bytes: knobs), when it changes, at most every 200 ms
     03  sensor 0x15's reply (102 bytes: touch, strips, ADC, switches), the same
     04  bytes from the keybed UART, as they come
     05  the LED bit now lit: chain (0 A, 1 B, 2 C), bit (2 bytes)
   In (any cable): SysEx F0 7D 4D 4B <cmd> ... F7, or CC 80 = <cmd> on channel 16 with its
   arguments first in CC 81, 82, 83
     01 c h l  light only bit h << 7 | l of chain c     02  lights back to the UI
     03  walk: the next bit every second, C then A then B   04  stop walking
     05  send everything now                                 06 m  the display view on (1) or off
   Both page buttons held 2 s step through: display view, display view + LED walk, off.
   Display view: 1 the input bits, 2 the last keybed bytes, 3 and 4 the last changed bytes of
   0x12 and 0x15 (index, value), 9 the LED bit lit. */
#ifdef MK_PROBE
#include <string.h>
#include "probe.h"
#include "display.h"
#include "ni_sensor.h"
#include "panel.h"

int ni_link_send(uint32_t pkt);
int ni_link_room(void);
void lights_probe(int chain, int bit);   /* lights.c: one bit lit, chain -1 = the UI again */

#define SENSOR_MS 200
#define WALK_MS 1000
#define HOLD_MS 2000
static const uint16_t chain_bits[3] = {72, 80, 112};

static volatile uint32_t in_bits;
static uint8_t frame0[NI_KNOBS_LEN], frame1[NI_TOUCH_LEN];
static uint8_t *const frame[2] = {frame0, frame1};
static uint32_t shown[2];             /* a checksum of the frame last sent */
static volatile uint8_t frame_seq[2];
static uint8_t frame_len[2] = {NI_KNOBS_LEN, NI_TOUCH_LEN};
static volatile uint8_t kb[32];
static volatile uint32_t kb_head;
static uint32_t kb_tail, sent_bits = 0xFFFFFFFF;
static uint8_t seen_seq[2], last_idx[2], last_val[2], kb_last[3];
static uint32_t sensor_ms[2], walk_ms, hold_ms;
static uint8_t view, walking, force, held;
static int led_chain = -1, led_bit;

void probe_inputs(uint32_t bits) { in_bits = bits; }

void probe_sensor(int which, const uint8_t *r, int n)
{
    if (n > NI_TOUCH_LEN)
        n = NI_TOUCH_LEN;
    memcpy(frame[which], r, n);         /* torn reads only cost a stale byte in one message */
    frame_seq[which]++;
}

void probe_keybed(uint8_t b)
{
    kb[kb_head % sizeof kb] = b;
    kb_head++;
}

/* ---------- out: control changes on channel 16 ---------- */

#define RESERVE 8                       /* packets left for the core's own MIDI */

static void cc(uint8_t num, uint8_t val)
{
    ni_link_send(0x0B | 0xBF << 8 | (uint32_t)num << 16 | (uint32_t)(val & 0x7F) << 24);
}

static void stream(uint8_t type, const uint8_t *p, int n)
{
    uint8_t sum = 0;
    cc(15, type);
    for (int i = 0; i < n; i++) {
        cc((uint8_t)(16 + (p[i] >> 7)), p[i]);
        sum += p[i];
    }
    cc(14, sum);
}

static int fits(int n)                  /* room for a message of n payload bytes */
{
    return ni_link_room() >= n + 2 + RESERVE;
}

/* ---------- lights ---------- */

static void led(int chain, int bit)
{
    led_chain = chain, led_bit = bit;
    lights_probe(chain, bit);
    if (chain >= 0) {
        uint8_t p[3] = {(uint8_t)chain, (uint8_t)(bit >> 8), (uint8_t)bit};
        if (fits(3))
            stream(5, p, 3);
    }
}

static void walk_step(void)
{
    static const int8_t order[3] = {2, 0, 1};      /* C first: the button and strip lights */
    int k = 0;
    while (k < 3 && order[k] != led_chain)
        k++;
    if (k == 3)
        led(2, 0);
    else if (led_bit + 1 < chain_bits[led_chain])
        led(led_chain, led_bit + 1);
    else
        led(order[(k + 1) % 3], 0);
}

/* ---------- in: commands ---------- */

static void command(const uint8_t *m, int n)    /* m[0] = cmd, then its arguments */
{
    switch (m[0]) {
    case 1: if (n >= 4 && m[1] < 3) led(m[1], (m[2] << 7 | m[3]) % chain_bits[m[1]]); walking = 0; break;
    case 2: led(-1, 0); walking = 0; break;
    case 3: walking = 1; break;
    case 4: walking = 0; break;
    case 5: force = 1; break;
    case 6: if (n >= 2) view = m[1] ? 1 : 0; break;
    }
}

int probe_midi_in(const uint8_t pkt[4])
{
    static uint8_t m[16], arg[4];
    static int n, active;
    uint8_t cin = pkt[0] & 15;
    if (cin == 0x0B && pkt[1] == 0xBF && pkt[2] >= 80 && pkt[2] <= 83) {
        if (pkt[2] > 80) {
            arg[pkt[2] - 80] = pkt[3];
        } else {
            arg[0] = pkt[3];
            command(arg, 4);
        }
        return 1;
    }
    if (cin < 4 || cin > 7)
        return 0;
    int k = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : 1;
    if (pkt[1] == 0xF0)
        n = 0, active = 1;
    if (!active)
        return 0;
    for (int i = 0; i < k; i++)
        if (n < (int)sizeof m)
            m[n++] = pkt[1 + i];
    if (cin == 4)
        return 1;
    active = 0;
    if (n < 6 || m[1] != 0x7D || m[2] != 'M' || m[3] != 'K')
        return 0;
    command(m + 4, n - 5);
    return 1;
}

/* ---------- the display view ---------- */

int probe_display(void)
{
    return view;
}

static void hex(char *s, const uint8_t *p, int n)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++)
        s[2 * i] = h[p[i] >> 4], s[2 * i + 1] = h[p[i] & 15];
}

static void put(seg_display_t *d, int line, const char *s)
{
    memcpy(d->line[line], s, strlen(s) < 8 ? strlen(s) : 8);
}

static void draw(void)
{
    seg_display_t d[9];
    memset(d, 0, sizeof d);
    for (int i = 0; i < 9; i++)
        memset(d[i].line, ' ', sizeof d[i].line);
    uint32_t b = in_bits;
    uint8_t ib[4] = {(uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b};
    put(&d[0], 0, "INPUTS");
    hex(d[0].line[1], ib, 4);
    put(&d[1], 0, "KEYBED");
    hex(d[1].line[1], kb_last, 3);
    for (int s = 0; s < 2; s++) {
        put(&d[2 + s], 0, s ? "S15" : "S12");
        uint8_t v[2] = {last_idx[s], last_val[s]};
        hex(d[2 + s].line[1], v, 2);
    }
    if (led_chain >= 0) {
        char t[9] = "LED A";
        t[4] = (char)('A' + led_chain);
        put(&d[8], 0, t);
        uint8_t v[2] = {(uint8_t)(led_bit >> 8), (uint8_t)led_bit};
        hex(d[8].line[1], v, 2);
    }
    display_show(d);
}

/* ---------- main loop ---------- */

void probe_task(void)
{
    uint32_t now = hal_ms(), b = in_bits;
    /* both page buttons held 2 s: off -> display view -> view + LED walk -> off */
    if ((b >> 28 & 3) == 3) {
        if (!held && now - hold_ms >= HOLD_MS) {
            held = 1;
            int mode = view ? (walking ? 2 : 1) : 0;
            mode = (mode + 1) % 3;
            view = mode >= 1;
            walking = mode == 2;
            if (!mode)
                led(-1, 0);
        }
    } else {
        held = 0;
        hold_ms = now;
    }
    if (walking && now - walk_ms >= WALK_MS) {
        walk_ms = now;
        walk_step();
    }
    if ((b != sent_bits || force) && fits(4)) {
        uint8_t p[4] = {(uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(b >> 16), (uint8_t)(b >> 24)};
        stream(1, p, 4);
        sent_bits = b;
    }
    while (kb_tail != kb_head && fits(1)) {
        uint8_t x = kb[kb_tail % sizeof kb];
        kb_tail++;
        kb_last[0] = kb_last[1], kb_last[1] = kb_last[2], kb_last[2] = x;
        stream(4, &x, 1);
    }
    for (int s = 0; s < 2; s++) {
        if (frame_seq[s] == seen_seq[s] && !force)
            continue;
        if (!force && now - sensor_ms[s] < SENSOR_MS)
            continue;
        static uint8_t prev[2][8];          /* the first bytes, to name a changed one */
        uint32_t sum = 0;
        for (int i = 0; i < frame_len[s]; i++) {
            sum = sum * 31 + frame[s][i];
            if (i < 8 && frame[s][i] != prev[s][i])
                last_idx[s] = (uint8_t)i, last_val[s] = frame[s][i], prev[s][i] = frame[s][i];
        }
        seen_seq[s] = frame_seq[s];
        if ((sum == shown[s] && !force) || !fits(frame_len[s]))
            continue;
        shown[s] = sum;
        stream((uint8_t)(2 + s), frame[s], frame_len[s]);
        sensor_ms[s] = now;
    }
    force = 0;
    static uint32_t drawn;
    if (view && now - drawn >= 50) {
        drawn = now;
        draw();
    }
}
#endif
