/* M32 keybed, button matrix, panel buttons and 4D-encoder rotation,
   scanned every 100 us from the platform tick. Events go through a
   single-producer queue to board_poll. docs/hardware/m32.md sections 1-3. */
#include "keybed.h"
#include "hw.h"
#include "vel_table.h"

#define GPIOA_CRL  REG(0x40010800)
#define GPIOA_IDR  REG(0x40010808)
#define GPIOA_BSRR_ REG(0x40010810)
#define GPIOB_CRL  REG(0x40010C00)
#define GPIOB_CRH  REG(0x40010C04)
#define GPIOB_IDR  REG(0x40010C08)
#define GPIOB_BSRR REG(0x40010C10)
#define GPIOB_BRR  REG(0x40010C14)
#define GPIOC_CRL  REG(0x40011000)
#define GPIOC_CRH  REG(0x40011004)
#define GPIOC_IDR  REG(0x40011008)
#define GPIOC_BSRR REG(0x40011010)
#define TICK_US 100
#define QLEN 128

static uint32_t ticks;           /* 100 us units */
static uint8_t btn_div;
uint16_t keybed_dropped;

/* event queue: the tick interrupt produces, the main loop consumes */
static mk_event_t queue[QLEN];
static volatile uint8_t q_head, q_tail;

static void push(uint8_t type, uint8_t id, int16_t value)
{
    uint8_t next = (q_head + 1) & (QLEN - 1);
    if (next == q_tail) {
        keybed_dropped++;
        return;
    }
    queue[q_head] = (mk_event_t){type, id, value};
    __asm volatile("dmb" ::: "memory");     /* entry before index */
    q_head = next;
}

int keybed_next(mk_event_t *ev)
{
    if (q_tail == q_head)
        return 0;
    __asm volatile("dmb" ::: "memory");
    *ev = queue[q_tail];
    __asm volatile("" ::: "memory");
    q_tail = (q_tail + 1) & (QLEN - 1);
    return 1;
}

/* ---------- matrix ---------- */

static void select_row(int row)
{
    /* PB14 = bank (1: rows 0-7), PB0..PB2 = address */
    uint32_t a = row & 7, set = a, clr = ~a & 7;
    if (row < 8) set |= 1 << 14; else clr |= 1 << 14;
    GPIOB_BSRR = set | (clr << 16);
}

static uint8_t read_row(int row)
{
    select_row(row);
    for (volatile int i = 0; i < 30; i++)
        ;
    uint8_t v = ~(GPIOC_IDR >> 8);   /* active low -> 1 = closed */
    /* drive the column lines high briefly so they settle fast for the next row */
    GPIOC_BSRR = 0xFF00;
    GPIOC_CRH = 0x33333333;
    GPIOC_CRH = 0x88888888;
    return v;
}

/* ---------- keybed ---------- */

static uint8_t key_raw[8], key_cnt[8], key_stable[8];
static uint8_t kstate[32];       /* 0 idle, 1 first contact, 2 down */
static uint32_t kt[32];

static const uint32_t black_mask =
    /* key 0 = F: black keys at F# G# A# C# D# */
    (1u << 1) | (1u << 3) | (1u << 5) | (1u << 8) | (1u << 10) |
    (1u << 13) | (1u << 15) | (1u << 17) | (1u << 20) | (1u << 22) |
    (1u << 25) | (1u << 27) | (1u << 29);

static uint8_t velocity(int key, uint32_t dt_ticks)
{
    /* stock firmware: 200 us units plus travel compensation 6 (white) / 12 (black) */
    uint32_t raw = (dt_ticks * TICK_US + 100) / 200 + ((black_mask >> key) & 1 ? 12 : 6);
    return vel_table[raw > 255 ? 255 : raw];
}

static void key_contact(int key, int contact, int closed)
{
    uint8_t *s = &kstate[key];
    if (contact == 0) {
        if (closed && *s == 0) {
            *s = 1;
            kt[key] = ticks;
        } else if (!closed && *s == 1) {
            *s = 0;                          /* half press */
        } else if (!closed && *s == 2) {
            *s = 0;
            push(EV_KEY, key, 0);
        }
    } else {
        if (closed && *s == 1) {
            *s = 2;
            push(EV_KEY, key, velocity(key, ticks - kt[key]));
        } else if (!closed && *s == 2) {
            kt[key] = ticks;                 /* release starts */
        }
    }
}

static void scan_keys(void)
{
    for (int row = 0; row < 8; row++) {
        uint8_t v = read_row(row);
        if (v != key_raw[row]) {
            key_raw[row] = v;
            key_cnt[row] = 0;
            continue;
        }
        if (key_cnt[row] < 2) {
            key_cnt[row]++;
            continue;
        }
        uint8_t changed = v ^ key_stable[row];
        if (!changed)
            continue;
        key_stable[row] = v;
        int pair = row >> 1, contact = row & 1;
        for (int b = 0; b < 8; b++)
            if (changed & (1 << b))
                key_contact(BOARD_KEYS - 1 - (pair * 8 + b), contact, (v >> b) & 1);
    }
}

/* ---------- buttons ---------- */

/* matrix position -> CTL_* (the M32's own indices happen to equal them) */
static const uint8_t btn_map[4][8] = {
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0xFF, 0xFF},
    {0x10, 0x11, 0x12, 0x0C, 0x0D, 0xFF, 0x0E, 0x0F},
    {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x14, 0x13},
    {0x16, 0x15, 0x17, 0x18, 0x19, 0x1A, 0xFF, 0xFF},
};
static uint8_t btn_raw[4], btn_cnt[4], btn_stable[4];

static void scan_buttons(void)
{
    uint8_t g[4];
    for (int i = 0; i < 3; i++)
        g[i] = read_row(8 + i);
    uint32_t pa = GPIOA_IDR, pb = GPIOB_IDR;
    g[3] = (~pa & 0x0F) | (!(pb & (1 << 13)) << 4) | (!!(pb & (1 << 12)) << 5);

    for (int i = 0; i < 4; i++) {
        if (g[i] != btn_raw[i]) {
            btn_raw[i] = g[i];
            btn_cnt[i] = 0;
            continue;
        }
        if (btn_cnt[i] < 3) {
            btn_cnt[i]++;
            continue;
        }
        uint8_t changed = g[i] ^ btn_stable[i];
        btn_stable[i] = g[i];
        for (int b = 0; b < 8; b++)
            if ((changed & (1 << b)) && btn_map[i][b] != 0xFF)
                push(EV_BUTTON, btn_map[i][b], (g[i] >> b) & 1);
    }
}

/* ---------- 4D encoder rotation (PC6/PC7 quadrature) ---------- */

static uint8_t enc_prev, enc_last_raw;
static int8_t enc_acc;           /* quadrature edges; 2 per detent (measured) */

static void scan_encoder(void)
{
    static const int8_t qtab[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};
    uint8_t raw = (GPIOC_IDR >> 6) & 3;
    if (raw != enc_last_raw) {            /* debounce: needs one repeat */
        enc_last_raw = raw;
        return;
    }
    if (raw == enc_prev)
        return;
    enc_acc += qtab[(enc_prev << 2) | raw];
    enc_prev = raw;
}

/* ---------- ISR and init ---------- */

void keybed_tick(void)
{
    ticks++;
    scan_keys();
    scan_encoder();
    if (++btn_div >= 10) {               /* 1 ms */
        btn_div = 0;
        scan_buttons();
        int detents = enc_acc / 2;
        if (detents) {
            enc_acc -= detents * 2;
            push(EV_ENCODER, 0, detents);
        }
    }
}

void keybed_pins(void)
{
    RCC_APB2ENR |= (1 << 2) | (1 << 3) | (1 << 4);        /* GPIOA, B, C */

    /* PB0-2 and PB14 outputs (row select) */
    GPIOB_CRL = (GPIOB_CRL & ~0x00000FFFu) | 0x00000333;
    GPIOB_CRH = (GPIOB_CRH & ~0x0FFF0000u) | 0x03840000;  /* PB14 out, PB13 IPU, PB12 floating */
    GPIOB_BSRR = 1 << 13;

    /* PC8-15 column inputs with pull-up, PC6/PC7 encoder inputs with pull-up */
    GPIOC_BSRR = 0xFF00 | (3 << 6);
    GPIOC_CRH = 0x88888888;
    GPIOC_CRL = (GPIOC_CRL & ~0xFF000000u) | 0x88000000;

    /* PA0-3 buttons, pull-up */
    GPIOA_CRL = (GPIOA_CRL & ~0x0000FFFFu) | 0x00008888;
    GPIOA_BSRR_ = 0x000F;

}

int keybed_boot_combo(void)
{
    /* Shift (row 8 bit 0) and Browser (row 9 bit 0) held at power-up */
    int hits = 0;
    for (int i = 0; i < 20; i++) {
        uint8_t a = read_row(8), b = read_row(9);
        hits += (a & 1) && (b & 1);
        for (volatile int d = 0; d < 2000; d++)
            ;
    }
    return hits >= 18;
}

void keybed_init(void)
{
    keybed_pins();
    enc_prev = enc_last_raw = (GPIOC_IDR >> 6) & 3;
    /* settle the debouncers on the current state so nothing fires at boot */
    for (int i = 0; i < 8; i++)
        key_stable[i] = key_raw[i] = read_row(i);
}
