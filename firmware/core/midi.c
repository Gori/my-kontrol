#include "midi.h"

/* ---------- out ---------- */

#define QLEN 256                     /* packets, power of two */

static uint8_t q[QLEN][4];
static uint16_t q_head, q_tail;
uint16_t midi_tx_dropped;

int midi_tx_free(void)
{
    return QLEN - 1 - ((q_head - q_tail) & (QLEN - 1));
}

int midi_tx_peek(uint8_t pkt[4])
{
    if (q_head == q_tail)
        return 0;
    for (int i = 0; i < 4; i++)
        pkt[i] = q[q_tail][i];
    return 1;
}

void midi_tx_pop(void)
{
    if (q_head != q_tail)
        q_tail = (q_tail + 1) & (QLEN - 1);
}

static void put(uint8_t cable, uint8_t cin, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t *p = q[q_head];
    p[0] = cable << 4 | cin;
    p[1] = a;
    p[2] = b;
    p[3] = c;
    q_head = (q_head + 1) & (QLEN - 1);
}

/* Code index number for a message starting with status byte s. */
static uint8_t cin_for(uint8_t s, uint16_t n)
{
    if (s < 0xF0)
        return s >> 4;
    if (s >= 0xF8 || s == 0xF6)
        return s >= 0xF8 ? 0xF : 0x5;
    if (s == 0xF1 || s == 0xF3)
        return 0x2;
    if (s == 0xF2)
        return 0x3;
    (void)n;
    return 0xF;
}

int midi_send(uint8_t port, const uint8_t *m, uint16_t n)
{
    if (!n || port >= PORT_COUNT)
        return -1;
    if (m[0] != 0xF0) {
        if (midi_tx_free() < 1) {
            midi_tx_dropped++;
            return -1;
        }
        put(port, cin_for(m[0], n), m[0], n > 1 ? m[1] : 0, n > 2 ? m[2] : 0);
        return 0;
    }
    if (midi_tx_free() < (n + 2) / 3) {
        midi_tx_dropped++;
        return -1;
    }
    /* sysex: CIN 4 for each full 3-byte chunk, 5/6/7 for the last 1/2/3 bytes */
    uint16_t i = 0;
    while (n - i > 3) {
        put(port, 0x4, m[i], m[i + 1], m[i + 2]);
        i += 3;
    }
    uint16_t r = n - i;
    put(port, 0x4 + r, m[i], r > 1 ? m[i + 1] : 0, r > 2 ? m[i + 2] : 0);
    return 0;
}

int midi_send3(uint8_t port, uint8_t status, uint8_t a, uint8_t b)
{
    uint8_t m[3] = {status, a, b};
    uint8_t hi = status & 0xF0;
    return midi_send(port, m, hi == 0xC0 || hi == 0xD0 ? 2 : 3);
}

/* ---------- in ---------- */

typedef struct {
    uint8_t status, n, need;
    uint8_t data[2];
    uint8_t sx[MIDI_SYSEX_MAX];
    uint16_t sx_len;
    uint8_t in_sx, sx_overflow;
} parser_t;

static parser_t parsers[PORT_COUNT];

void midi_rx_reset(void)
{
    for (int i = 0; i < PORT_COUNT; i++)
        parsers[i].status = parsers[i].in_sx = 0;
}

static uint8_t data_len(uint8_t s)
{
    uint8_t hi = s & 0xF0;
    if (hi == 0xC0 || hi == 0xD0 || s == 0xF1 || s == 0xF3)
        return 1;
    if (s == 0xF2 || s < 0xF0)
        return 2;
    return 0;
}

static void parse_byte(uint8_t port, uint8_t b)
{
    parser_t *p = &parsers[port];
    if (b >= 0xF8) {                         /* realtime: may appear anywhere */
        app_midi_realtime(port, b);
        return;
    }
    if (b == 0xF0) {
        p->in_sx = 1;
        p->sx_overflow = 0;
        p->sx_len = 0;
        p->sx[p->sx_len++] = b;
        p->status = 0;
        return;
    }
    if (p->in_sx) {
        if (b == 0xF7) {
            p->in_sx = 0;
            if (!p->sx_overflow && p->sx_len < MIDI_SYSEX_MAX) {
                p->sx[p->sx_len++] = b;
                app_midi_sysex(port, p->sx, p->sx_len);
            }
            return;
        }
        if (!(b & 0x80)) {
            if (p->sx_len < MIDI_SYSEX_MAX - 1)
                p->sx[p->sx_len++] = b;
            else
                p->sx_overflow = 1;
            return;
        }
        p->in_sx = 0;                        /* any other status aborts the sysex */
    }
    if (b & 0x80) {
        p->status = b;
        p->n = 0;
        p->need = data_len(b);
        if (!p->need) {                      /* F6 tune request, stray F7 */
            if (b == 0xF6)
                app_midi_message(port, b, 0, 0);
            p->status = 0;
        }
        return;
    }
    if (!p->status)
        return;
    p->data[p->n++] = b;
    if (p->n >= p->need) {
        app_midi_message(port, p->status, p->data[0], p->need > 1 ? p->data[1] : 0);
        p->n = 0;
        if (p->status >= 0xF0)
            p->status = 0;                   /* no running status for system common */
    }
}

void midi_rx_packet(const uint8_t pkt[4])
{
    static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    uint8_t port = pkt[0] >> 4, n = len[pkt[0] & 0xF];
    if (port >= PORT_COUNT)
        port = PORT_KEYS;
    for (int i = 0; i < n; i++)
        parse_byte(port, pkt[1 + i]);
}
