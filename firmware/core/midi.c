#include "midi.h"
#include "hal.h"

#if BOARD_MIDI_DIN
#define USB_CABLES 3                 /* KEYS, DAW, EXT */
#define PARSERS PORT_COUNT
#else
#define USB_CABLES 2
#define PARSERS PORT_EXT             /* no 5-pin: no EXT cable, no DIN parser */
#endif

/* ---------- out ---------- */

#ifndef BOARD_MIDI_QLEN
#define BOARD_MIDI_QLEN 256
#endif
#define QLEN BOARD_MIDI_QLEN         /* packets, power of two */

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

/* ---------- 5-pin out ---------- */

#if BOARD_MIDI_DIN
#define DIN_QLEN 256                 /* bytes, power of two: 80 ms of wire */
#define HOLD_LEN 64
#define NONE 0xFF
#define STALL_MS 100                 /* a sysex that stops coming is ended after this long */

static uint8_t din_q[DIN_QLEN];
static uint16_t din_head, din_tail;
static uint8_t din_status;           /* running status: the channel status last queued, 0 = none */
static uint8_t din_owner = NONE;     /* port whose sysex is streaming out, byte by byte */
static uint8_t hold[HOLD_LEN], hold_n;   /* messages waiting for that sysex to end: length, bytes */
static uint32_t din_idle_ms;
uint16_t midi_din_dropped;

/* EXT goes out the 5-pin, so it waits in a queue of its own while the
   5-pin is busy; USB input (the DAW port too) goes on meanwhile. */
#ifndef BOARD_DIN_EXT_QUEUE
#define BOARD_DIN_EXT_QUEUE 2048         /* bytes, power of two */
#endif
_Static_assert(!(BOARD_DIN_EXT_QUEUE & (BOARD_DIN_EXT_QUEUE - 1)), "BOARD_DIN_EXT_QUEUE: a power of two");
static uint8_t ext_q[BOARD_DIN_EXT_QUEUE];
static uint16_t ext_head, ext_tail;

static int din_free(void)
{
    return DIN_QLEN - 1 - ((din_head - din_tail) & (DIN_QLEN - 1));
}

/* while a sysex streams, one byte stays free for its F7 */
static int din_room(void)
{
    return din_free() - (din_owner != NONE);
}

static void din_put(uint8_t b)
{
    din_q[din_head] = b;
    din_head = (din_head + 1) & (DIN_QLEN - 1);
}

/* Whole messages or nothing, like USB. Realtime bytes leave running status
   alone; anything else from F0 up cancels it. */
static int din_queue(const uint8_t *m, uint16_t n)
{
    int skip = m[0] < 0xF0 && m[0] == din_status;
    if (din_room() < n - skip) {
        midi_din_dropped++;
        return -1;
    }
    if (m[0] < 0xF8)
        din_status = m[0] < 0xF0 ? m[0] : 0;
    for (uint16_t i = skip; i < n; i++)
        din_put(m[i]);
    return 0;
}

/* the streaming sysex is over, or cut short: F7, then what waited */
static void din_close(void)
{
    din_put(0xF7);                   /* the byte kept free for it */
    din_owner = NONE;
    for (int i = 0; i < hold_n; i += 1 + hold[i])
        din_queue(hold + i + 1, hold[i]);
    hold_n = 0;
}

/* Nothing goes inside a streaming sysex but realtime bytes; the rest waits.
   If too much waits, the sysex is cut short: playing comes first. */
static int din_send(const uint8_t *m, uint16_t n)
{
    if (!n || !(m[0] & 0x80))
        return -1;
    if (din_owner != NONE && m[0] < 0xF8) {
        if (hold_n + 1 + n <= HOLD_LEN) {
            hold[hold_n++] = n;
            for (uint16_t i = 0; i < n; i++)
                hold[hold_n++] = m[i];
            return 0;
        }
        midi_din_dropped++;
        din_close();
    }
    return din_queue(m, n);
}

int midi_din_sysex(uint8_t src, uint8_t b)
{
    if (b == 0xF0) {
        if (din_owner != NONE || din_free() < 2) {   /* another one is streaming, or full */
            midi_din_dropped++;
            return -1;
        }
        din_owner = src;
        din_status = 0;
        din_idle_ms = 0;
        din_put(b);
        return 0;
    }
    if (din_owner != src)
        return -1;                   /* not started, or cut short */
    din_idle_ms = 0;
    if (b == 0xF7) {
        din_close();
        return 0;
    }
    if (din_room() < 1) {            /* full: end it here, the line stays sane */
        midi_din_dropped++;
        din_close();
        return -1;
    }
    din_put(b);
    return 0;
}

static void ext_pump(void);

void midi_din_tick_ms(uint32_t ms)
{
    ext_pump();
    if (din_owner == NONE || din_head != din_tail)
        din_idle_ms = 0;
    else if ((din_idle_ms += ms) >= STALL_MS)
        din_close();
}

int midi_din_tx_byte(void)
{
    if (din_head == din_tail) {
        din_status = 0;              /* idle: the next burst starts with a status byte */
        return -1;
    }
    uint8_t b = din_q[din_tail];
    din_tail = (din_tail + 1) & (DIN_QLEN - 1);
    return b;
}
#endif

int midi_send(uint8_t port, const uint8_t *m, uint16_t n)
{
#if BOARD_MIDI_DIN
    if (port == PORT_DIN)
        return din_send(m, n);
#endif
    if (!n || port >= USB_CABLES)
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

#if BOARD_MIDI_DIN
/* 5-pin in to the computer: one sysex at a time, packed as it comes */
static uint8_t ux[3], ux_n, ux_state;    /* 0 idle, 1 passing, 2 cut short: F7 still owed */

int midi_usb_sysex(uint8_t port, uint8_t b)
{
    if (b == 0xF0) {
        ux_state = 1;
        ux_n = 0;
    } else if (ux_state != 1) {
        if (ux_state == 2 && b == 0xF7 && midi_tx_free() > 0) {
            put(port, 0x5, 0xF7, 0, 0);
            ux_state = 0;
        }
        return -1;
    }
    ux[ux_n++] = b;
    if (b != 0xF7 && ux_n < 3)
        return 0;
    if (midi_tx_free() < 1) {
        midi_tx_dropped++;
        ux_state = b == 0xF7 ? 0 : 2;
        return -1;
    }
    put(port, b == 0xF7 ? 0x4 + ux_n : 0x4, ux[0], ux_n > 1 ? ux[1] : 0, ux_n > 2 ? ux[2] : 0);
    ux_n = 0;
    if (b == 0xF7)
        ux_state = 0;
    return 0;
}
#endif

int midi_send3(uint8_t port, uint8_t status, uint8_t a, uint8_t b)
{
    uint8_t m[3] = {status, a, b};
    return midi_send(port, m, midi_msg_len(status));
}

/* ---------- keys route ---------- */

static uint8_t keys_route = MIDI_TO_USB;

void midi_keys_route(uint8_t route)
{
    keys_route = route;
}

int midi_keys(const uint8_t *m, uint16_t n)
{
    int r = 0;
    if (keys_route & MIDI_TO_USB)
        r |= midi_send(PORT_KEYS, m, n);
#if BOARD_MIDI_DIN
    if (keys_route & MIDI_TO_DIN)
        r |= din_send(m, n);
#endif
    return r;
}

int midi_keys3(uint8_t status, uint8_t a, uint8_t b)
{
    uint8_t m[3] = {status, a, b};
    return midi_keys(m, midi_msg_len(status));
}

/* ---------- in ---------- */

typedef struct {
    uint8_t status, n, need;
    uint8_t data[2];
    uint8_t sx[MIDI_SYSEX_MAX];
    uint16_t sx_len;
    uint8_t in_sx, sx_overflow;
} parser_t;

static parser_t parsers[PARSERS];

void midi_rx_reset(void)
{
    for (int i = 0; i < PARSERS; i++)
        parsers[i].status = parsers[i].in_sx = 0;
#if BOARD_MIDI_DIN
    ext_head = ext_tail = 0;
#endif
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

uint8_t midi_msg_len(uint8_t s)
{
    return s == 0xF0 ? 0 : 1 + data_len(s);
}

static void parse_byte(uint8_t port, uint8_t b)
{
    parser_t *p = &parsers[port];
    if (b >= 0xF8) {                         /* realtime: may appear anywhere */
        app_midi_realtime(port, b);
        return;
    }
#if BOARD_MIDI_DIN
    /* EXT and DIN pass sysex on byte by byte, any length */
    if (port >= PORT_EXT && (b == 0xF0 || p->in_sx)) {
        if (p->in_sx && !(b & 0x80)) {
            app_midi_sysex_byte(port, b);
            return;
        }
        if (p->in_sx)
            app_midi_sysex_byte(port, 0xF7);         /* any status ends it, F0 too */
        p->in_sx = b == 0xF0;
        p->status = 0;
        if (b == 0xF0)
            app_midi_sysex_byte(port, b);
        if (b == 0xF0 || b == 0xF7)
            return;
    }
#endif
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

#if BOARD_MIDI_DIN
/* 1 if this byte from EXT can be handled now without dropping or cutting
   anything on the 5-pin (a whole message is assumed to end with it) */
static int ext_fits(uint8_t b)
{
    if (b >= 0xF8)
        return din_room() >= 1;                         /* realtime: anywhere */
    if (b == 0xF0)
        return din_owner != PORT_DIN && din_free() >= 3;
    if (parsers[PORT_EXT].in_sx)
        return din_owner != PORT_DIN && din_room() >= 1;
    if (din_owner != NONE)
        return hold_n + 4 <= HOLD_LEN;
    return din_free() >= 3;
}

static void ext_pump(void)
{
    while (ext_head != ext_tail && ext_fits(ext_q[ext_tail])) {
        uint8_t b = ext_q[ext_tail];
        ext_tail = (ext_tail + 1) & (BOARD_DIN_EXT_QUEUE - 1);
        parse_byte(PORT_EXT, b);
    }
}
#endif

int midi_rx_packet(const uint8_t pkt[4])
{
    static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    uint8_t port = pkt[0] >> 4, n = len[pkt[0] & 0xF];
    if (port >= USB_CABLES)
        port = PORT_KEYS;
#if BOARD_MIDI_DIN
    if (port == PORT_EXT) {
        if (BOARD_DIN_EXT_QUEUE - 1 - ((ext_head - ext_tail) & (BOARD_DIN_EXT_QUEUE - 1)) < n)
            return 0;                        /* full: the packet waits in USB */
        for (int i = 0; i < n; i++) {
            ext_q[ext_head] = pkt[1 + i];
            ext_head = (ext_head + 1) & (BOARD_DIN_EXT_QUEUE - 1);
        }
        ext_pump();
        return 1;
    }
#endif
    for (int i = 0; i < n; i++)
        parse_byte(port, pkt[1 + i]);
    return 1;
}

#if BOARD_MIDI_DIN
void midi_din_rx_byte(uint8_t b)
{
    parse_byte(PORT_DIN, b);
}
#endif
