/* XS1 tile 0 next to NI's tile 1 (the hybrid bring-up image, `make BOARD=s49 HYBRID=1`), in
   place of tile0_link.c: NI's USB, DFU and 5-pin MIDI stay in, so NI's update path keeps
   working while our tile 0 runs on the keyboard. Developer builds only.

   What NI's tile 1 expects of tile 0 was observed on the emulator (tools/xcore/s49emu.py,
   NI's whole image, the channel traffic between the tiles); fw notes xmos.md section 7 has
   the thread map. Nothing of NI's code or data is used: we answer the same requests with our
   own values.

   - Start-up (the XC runtime's channel set-up; tile 1 is the master): tile 1 sends its id
     to our chanend 0, we answer with ours, take words until a 0, send 10 more chanend ids,
     then get 10 [return id, index, remote id] connections and acknowledge each (ACK, END).
   - Every value then moves as an XC channel transfer: the sender sends END, the receiver
     answers END, the sender sends the data tokens and END, the receiver answers END.
   - Our ch[4] <-> tile 1's router: we poll with 0x0a and our count of waiting MIDI packets
     (0 or 1), it answers with a word 0 and a word that is 0, or 0xC000001F followed by a
     transfer [count, packets] (MIDI from the 5-pin IN or USB for us), after which it waits for
     a word 2 and one word per packet from us: NI's tile 0 answers 1 for a packet after which
     it wants a template block exchange (seen for notes) and 0 otherwise; we always send 0
     (seen under a burst of 5-pin input on the emulator). With a count of 1 it
     then asks on ch[9] (0xC000001D) and we send [1, the USB-MIDI packet]. One packet per
     poll and at most one poll per millisecond, as NI's tile 0 does.
   - Our ch[5] <-> tile 1's USB/HID thread: first it sends a word 0 and waits for four
     transfers: three tables (a count word, 0x70, 0x50, 0x48, and that many bytes; NI's are
     LED maps, ours are zeros) and a byte 7. Then, every 500 ms while it is idle, we poll it
     with a word 0, as NI's tile 0 does; it answers with its LED state (four tables and a
     status word) and sends again as long as the status is not 0. We take and drop it,
     paced to every 4 ms as NI's tile 0. Its other threads wait for these polls: tile 1
     does not answer on ch[4] before the first. In update mode (NI's DFU) it starts with
     0x40000000 instead and wants no tables; after our first poll it sends 0xC0000009, which
     NI's tile 0 answers with a byte 7, and so do we. Its other threads wait on
     this: until the first of these polls, tile 1 does not answer on ch[4].
   - We start our transfers with END and serve the other channels until tile 1 answers, so
     tile 1 never waits on us, whatever it is doing.
   - Our ch[8] <-> tile 1 chanend 9: no traffic seen; anything is taken and dropped.
   Two threads serve them, ch[5]/ch[8] (NI's USB and DFU thread) apart from ch[4]/ch[9] (the
   MIDI side), started before the board is touched, so no driver and no MIDI surprise can
   leave NI's USB thread waiting; an END that does not come is never a trap (xc_end).
   Nothing here writes the flash (settings are not kept: they last until power-off). */
#include <string.h>
#include "app.h"
#include "hal.h"
#include "kbd.h"
#include "link.h"
#include "midi.h"
#include "usb.h"
#include "xs1.h"
#ifdef MK_PROBE
#include "probe.h"
#endif

#define CT_ACK 3
#define NCH 10
#define POLL_IDLE_MS 5                  /* NI idles at 500 ms but polls at once with data */
#define HID_POLL_MS 500                 /* as NI's tile 0 */
#define HID_PACE_MS 4
#define CMD_POLL 0x0Au
#define CMD_PULL 0xC000001Du
#define CMD_INCOMING 0xC000001Fu
#define HID_ASK_ID 0xC0000009u

static uint32_t ch[NCH];
static volatile uint8_t ni_update;      /* tile 1 is in update mode */
static void cmd_thread(uint32_t arg), hid_thread(uint32_t arg);
static uint32_t cmd_stack[192], hid_stack[176];

/* ---------- single producer, single consumer packet rings ---------- */

#define RING 32
typedef struct {
    volatile uint32_t buf[RING];
    volatile uint32_t head, tail;
} ring_t;

static ring_t to_ni, from_ni;          /* main -> NI thread, NI thread -> main */

static int ring_put(ring_t *r, uint32_t v)
{
    if (r->head - r->tail >= RING)
        return 0;
    r->buf[r->head % RING] = v;
    r->head++;
    return 1;
}

static int ring_get(ring_t *r, uint32_t *v)
{
    if (r->tail == r->head)
        return 0;
    *v = r->buf[r->tail % RING];
    r->tail++;
    return 1;
}

int ni_link_send(uint32_t pkt)        /* a USB-MIDI packet, p[0] | p[1] << 8 ...; 0 = full */
{
    return ring_put(&to_ni, pkt);
}

int ni_link_room(void)
{
    return RING - (int)(to_ni.head - to_ni.tail);
}

/* ---------- XC channel transfers ---------- */

volatile uint32_t ni_desync;            /* data where an END was due: dropped, not trapped */

/* the END that is due; data in its place (a request of tile 1's we do not know) is dropped
   up to the next control token, so a surprise never traps the thread tile 1 depends on */
static void xc_end(uint32_t c)
{
    if (!xs1_testct(c)) {
        ni_desync++;
        while (!xs1_testct(c))
            (void)xs1_int(c);
    }
    (void)xs1_inct(c);
}

static void xc_send(uint32_t c, const uint8_t *p, uint32_t n)
{
    xs1_outct(c, CT_END);
    xc_end(c);
    for (uint32_t i = 0; i < n; i++)
        xs1_outt(c, p[i]);
    xs1_outct(c, CT_END);
    xc_end(c);
}

static void xc_send_word(uint32_t c, uint32_t w)
{
    uint8_t b[4] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
    xc_send(c, b, 4);
}

/* a transfer the other side started (its END is waiting): the data tokens, at most max kept */
static uint32_t xc_recv(uint32_t c, uint8_t *p, uint32_t max)
{
    uint32_t n = 0;
    xc_end(c);
    xs1_outct(c, CT_END);
    while (!xs1_testct(c)) {
        uint8_t b = (uint8_t)xs1_int(c);
        if (n < max)
            p[n] = b;
        n++;
    }
    xc_end(c);
    xs1_outct(c, CT_END);
    return n;
}

static uint32_t word_of(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* ---------- start-up: the XC channel set-up, tile 1 the master ---------- */

void tile0_link_open(void)
{
    uint32_t c0 = xs1_chanend0;             /* chanend 0 (crt0.s): 0x80000002 */
    uint32_t master = xs1_in(c0);
    xs1_chkct(c0, CT_END);
    xs1_setd(c0, master);
    xs1_out(c0, c0);
    xs1_outct(c0, CT_END);
    for (;;) {
        uint32_t w = xs1_in(c0);
        xs1_chkct(c0, CT_END);
        if (!w)
            break;
    }
    for (int i = 0; i < NCH; i++) {
        ch[i] = xs1_getr(RES_CHANEND);
        xs1_setd(ch[i], ch[i] | 0xFF00);
        xs1_out(c0, ch[i]);
    }
    xs1_outct(c0, CT_END);
    for (int i = 0; i < NCH; i++) {
        uint32_t ret = xs1_in(c0);
        xs1_setd(c0, ret);
        uint32_t idx = xs1_in(c0), remote = xs1_in(c0);
        xs1_chkct(c0, CT_END);
        xs1_outct(c0, CT_ACK);
        xs1_outct(c0, CT_END);
        if (remote && idx < NCH)
            xs1_setd(ch[idx], remote);
    }
    xs1_freer(c0);
    /* from here tile 1 waits on our polls for its USB and DFU: served before anything
       touches the board (a driver that hangs must not take NI's update path with it) */
    xs1_thread_start(hid_thread, 0, hid_stack, sizeof hid_stack / 4);
    xs1_thread_start(cmd_thread, 0, cmd_stack, sizeof cmd_stack / 4);
}

/* ---------- the thread that serves tile 1 ---------- */

/* wait for a token on a, b, c or d, or timer tmr passing t: 0..3, 4 = time */
static int wait5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t tmr, uint32_t t)
{
    int r;
    asm volatile(
        "clre\n"
        "ldap r11, .Lw5a%=\n" "setv res[%1], r11\n" "eeu res[%1]\n"
        "ldap r11, .Lw5b%=\n" "setv res[%2], r11\n" "eeu res[%2]\n"
        "ldap r11, .Lw5c%=\n" "setv res[%3], r11\n" "eeu res[%3]\n"
        "ldap r11, .Lw5d%=\n" "setv res[%4], r11\n" "eeu res[%4]\n"
        "setd res[%5], %6\n" "setc res[%5], 9\n"
        "ldap r11, .Lw5t%=\n" "setv res[%5], r11\n" "eeu res[%5]\n"
        "waiteu\n"
        ".Lw5a%=:\n" "ldc %0, 0\n" "bu .Lw5x%=\n"
        ".Lw5b%=:\n" "ldc %0, 1\n" "bu .Lw5x%=\n"
        ".Lw5c%=:\n" "ldc %0, 2\n" "bu .Lw5x%=\n"
        ".Lw5d%=:\n" "ldc %0, 3\n" "bu .Lw5x%=\n"
        ".Lw5t%=:\n" "ldc %0, 4\n"
        ".Lw5x%=:\n" "clre\n" "setc res[%5], 1\n"
        : "=&r"(r) : "r"(a), "r"(b), "r"(c), "r"(d), "r"(tmr), "r"(t) : "r11", "memory");
    return r;
}

static uint8_t rx[64];
static uint32_t committed;             /* the packet announced in the last poll */
static uint8_t pull_due;

static void incoming(uint32_t c)        /* 0xC000001F, then [count, packets] */
{
    uint32_t n = xc_recv(c, rx, sizeof rx);
    if (n < 4)
        return;
    uint32_t count = word_of(rx);
    for (uint32_t i = 0; i < count && 4 + 4 * i + 4 <= n && 4 + 4 * i + 4 <= sizeof rx; i++) {
        const uint8_t *p = rx + 4 + 4 * i;   /* a little-endian packet word, sent MSB first */
        ring_put(&from_ni, (uint32_t)p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24);
    }
    /* then it waits for a word 2 and one word per packet: NI's tile 0 answers 1 for a packet
       that should start a template block exchange (which follows that word), 0 for none */
    xc_send_word(c, 2);
    for (uint32_t i = 0; i < count; i++)
        xc_send_word(c, 0);
}

/* the three tables: a word (the count) and that many bytes, here zeros, then a byte 7 */
static void tables(uint32_t c)
{
    static const uint8_t count[3] = {0x70, 0x50, 0x48};
    for (int k = 0; k < 3; k++) {
        xs1_outct(c, CT_END);
        xc_end(c);
        for (int i = 0; i < 4 + count[k]; i++)
            xs1_outt(c, i == 3 ? count[k] : 0);
        xs1_outct(c, CT_END);
        xc_end(c);
    }
    uint8_t seven = 7;
    xc_send(c, &seven, 1);
}

/* our part of a transfer we started, after tile 1's answering END: the data, END, END */
static void xc_finish(uint32_t c, uint32_t w)
{
    xc_end(c);
    for (int i = 24; i >= 0; i -= 8)
        xs1_outt(c, (uint8_t)(w >> i));
    xs1_outct(c, CT_END);
    xc_end(c);
}

/* tile 1's router (ch[4]) and MIDI pulls (ch[9]) */
static void cmd_thread(uint32_t arg)
{
    (void)arg;
    uint32_t tmr = xs1_getr(RES_TIMER), now = xs1_time(tmr);
    uint32_t next_cmd = now + POLL_IDLE_MS * TICKS_PER_MS;
    uint32_t cmd = ch[4], pull = ch[9], n = 0;
    uint8_t cmd_wait = 0;               /* we sent END; tile 1's answer starts the rest */
    for (;;) {
        switch (wait5(cmd, pull, pull, pull, tmr, next_cmd)) {
        case 0:
            if (cmd_wait) {             /* the poll: 0x0a, our count, two words back */
                cmd_wait = 0;
                xc_finish(cmd, CMD_POLL);
                xc_send_word(cmd, n);
                xc_recv(cmd, rx, 0);
                if (xc_recv(cmd, rx, 4) == 4 && word_of(rx) == CMD_INCOMING)
                    incoming(cmd);      /* MIDI for us follows */
            } else if (xc_recv(cmd, rx, 4) == 4 && word_of(rx) == CMD_INCOMING) {
                incoming(cmd);
            }
            /* the next poll a full millisecond after this exchange: tile 1 may start one of
               its own right after it, and must go first */
            now = xs1_time(tmr) + TICKS_PER_MS;
            if ((int32_t)(next_cmd - now) < 0)
                next_cmd = now;
            break;
        case 1:                         /* tile 1 pulls the announced packet */
        case 2:
        case 3:
            if (xc_recv(pull, rx, 4) == 4 && word_of(rx) == CMD_PULL) {
                uint8_t p[8] = {0, 0, 0, pull_due ? 1 : 0,
                                (uint8_t)(committed >> 24), (uint8_t)(committed >> 16),
                                (uint8_t)(committed >> 8), (uint8_t)committed};
                xc_send(pull, p, pull_due ? 8 : 4);
                pull_due = 0;
            }
            break;
        default:
            now = xs1_time(tmr);
            if (!cmd_wait) {
                if (!pull_due && ring_get(&to_ni, &committed))
                    pull_due = 1;
                n = pull_due;           /* again if it did not take it (it skips the pull
                                           when it had MIDI for us in the same poll) */
                if (!xs1_ready(cmd)) {  /* (tile 1 starting something goes first) */
                    xs1_outct(cmd, CT_END);
                    cmd_wait = 1;
                }
                next_cmd = now + (n || to_ni.head != to_ni.tail ? 1 : POLL_IDLE_MS) * TICKS_PER_MS;
            } else {
                next_cmd = now + TICKS_PER_MS;
            }
            break;
        }
    }
}

/* tile 1's USB/HID thread (ch[5]) and ch[8]: on a thread of its own, so nothing on the
   MIDI side can keep NI's USB and DFU thread waiting */
static void hid_thread(uint32_t arg)
{
    (void)arg;
    uint32_t tmr = xs1_getr(RES_TIMER), now = xs1_time(tmr);
    uint32_t next_hid = now + HID_POLL_MS * TICKS_PER_MS;
    uint32_t hid_hold = now;            /* after its LED state: answer it again from here */
    uint32_t hid = ch[5], other = ch[8];
    uint8_t hid_wait = 0, started = 0, hid_idle = 0;   /* idle: it waits for a poll */
    static uint8_t hrx[8];
    for (;;) {
        uint32_t next = hid_idle ? next_hid : now + 100 * TICKS_PER_MS;
        int hold = (int32_t)(xs1_time(tmr) - hid_hold) < 0;
        if (hold && (int32_t)(hid_hold - next) < 0)
            next = hid_hold;
        switch (wait5(hold ? other : hid, other, other, other, tmr, next)) {
        case 0:
            if (hid_wait) {             /* our poll: a word 0, then it sends its LED state */
                hid_wait = 0;
                xc_finish(hid, 0);
            } else if (xc_recv(hid, hrx, sizeof hrx) == 4) {
                uint32_t w = word_of(hrx);
                if (!started) {         /* its first word: 0 asks for the tables; in update
                                           mode it sends 0x40000000 and wants none */
                    if (!w)
                        tables(hid);
                    else
                        ni_update = 1;  /* update mode: the core stops (xs1_hybrid_check) */
                    started = 1, hid_idle = 1;
                } else if (w == HID_ASK_ID) {
                    uint8_t seven = 7;  /* (update mode, after our poll): answered with 7 */
                    xc_send(hid, &seven, 1);
                    hid_idle = 1;
                } else {                /* a status word ends its LED state: 0 = it waits
                                           for our next poll, else it sends again */
                    hid_idle = w == 0;
                    hid_hold = xs1_time(tmr) + HID_PACE_MS * TICKS_PER_MS;
                }
            }
            break;
        case 1:
        case 2:
        case 3:
            xc_recv(other, hrx, 0);
            break;
        default:
            now = xs1_time(tmr);
            if (hid_idle && !hid_wait && (int32_t)(now - next_hid) >= 0) {
                if (!xs1_ready(hid)) {  /* only while it waits for one: no collision */
                    xs1_outct(hid, CT_END);
                    hid_wait = 1;
                    hid_idle = 0;
                }
                next_hid = now + HID_POLL_MS * TICKS_PER_MS;
            }
            break;
        }
    }
}

/* ---------- the core's side (usb.h, hal.h flash) ---------- */

void usb_init(void)
{
    xs1_hybrid_check();
    app_usb_mounted(1);                 /* tile 1 decides where our MIDI goes */
}

/* NI's tile 1 came up in update mode (its first word on ch[5]): the core stops, only the
   NI-link thread (and the panel threads) run on. From the main loop (hal_watchdog_feed). */
void xs1_update_mode(void) __attribute__((noreturn));

void xs1_hybrid_check(void)
{
    if (ni_update)
        xs1_update_mode();
}

void usb_task(void)
{
    uint8_t p[4];
    uint32_t v;
    while (ring_get(&from_ni, &v)) {
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
#ifdef MK_PROBE
        if (probe_midi_in(p))
            continue;
#endif
        midi_rx_packet(p);
    }
    while (midi_tx_peek(p)) {           /* dropped when tile 1 is slower than the core */
        midi_tx_pop();
        ni_link_send(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    }
#if BOARD_MIDI_DIN
    while (midi_din_tx_byte() >= 0)     /* NI's tile 1 sends our packets to the 5-pin port */
        ;
#endif
#ifdef MK_PROBE
    probe_task();
#endif
}

void hal_idle(void)
{
    static uint32_t tmr;
    if (!tmr)
        tmr = xs1_getr(RES_TIMER);
    uint32_t now = xs1_time(tmr);
    xs1_wait_until(tmr, now + TICKS_PER_MS - now % TICKS_PER_MS);
}

/* the settings: not kept (nothing reaches NI's flash); they last until power-off in RAM */
void hal_flash_read(uintptr_t addr, void *dst, uint32_t n)
{
    (void)addr;
    memset(dst, 0xFF, n);
}

int hal_flash_erase(uintptr_t addr) { (void)addr; return 0; }
int hal_flash_write(uintptr_t addr, const void *src, uint32_t len) { (void)addr, (void)src, (void)len; return 1; }

void tile0_flash_read(uint32_t spi, uint8_t *buf, uint32_t n)
{
    (void)spi;
    memset(buf, 0xFF, n);
}

int tile0_flash_erase(uint32_t spi) { (void)spi; return 1; }
int tile0_flash_write(uint32_t spi, const uint8_t *s, uint32_t len) { (void)spi, (void)s, (void)len; return 1; }
void tile0_link_wait(void) {}
int tile0_link_is_up(void) { return 0; }
int tile0_system(uint8_t cmd) { (void)cmd; return 1; }   /* NI's tile 1: its own update path */
void kbd_tap(uint8_t modifier, uint8_t key) { (void)modifier, (void)key; }
