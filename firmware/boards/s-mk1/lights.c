/* S MK1 lights (tile 0): three 74HC595 chains with 7-bit brightness (drivers/shift595.c,
   bit-angle modulation) and the display backlight. docs/hardware/s-mk1.md, Lights.

   Chains A (data 1G, latch 1E, 72 bits) and B (1J, 1L, 80 bits) share the clock on 1D
   (clock block 1, ref/64 = 1.5625 MHz): one thread shifts both, A padded to 80 bits. Chain
   C (1M, 1N, 112 bits) has its clock on 1I (clock block 2, ref/78 = 1.282 MHz) and its own
   thread, which also switches the backlight (1O) in the same planes. Data ports are
   buffered 8 bits wide; each latch is a one-clock pulse timed on the chain's clock, right
   after the last bit, because the clock runs all the time.

   Plane k lasts 2^k units: 122 clocks (78 us) for A/B, a 10 ms period; 128 clocks (100 us)
   for C, so its 112 bits fit in the shortest plane, a 12.8 ms period.

   Light guide: 3 bits per key (R, G, B), keys 0..23 on A and 24..48 on B, permuted inside
   each 24-bit group as NI's firmware addresses them (fw/kks/notes/xmos.md §8.3).
   Button and strip lights on chain C: which bit is which control is not known yet (TODO);
   ctl_bit below is empty, so board_light() does nothing until it is filled in. */
#include "panel.h"
#include "shift595.h"
#include "xs1.h"

#define AB_CLK PORT_1('D')
#define A_DATA PORT_1('G')
#define A_LATCH PORT_1('E')
#define B_DATA PORT_1('J')
#define B_LATCH PORT_1('L')
#define C_CLK  PORT_1('I')
#define C_DATA PORT_1('M')
#define C_LATCH PORT_1('N')
#define BACKLIGHT PORT_1('O')
#define AB_CB  CLKBLK(1)
#define C_CB   CLKBLK(2)

static uint8_t level_a[80], level_b[80], level_c[112];
static uint8_t backlight = 127;
static uint32_t ab_stack[64], c_stack[64];

static const uint8_t group[24] = {6, 7, 5, 3, 4, 2, 0, 1, 15, 13, 14, 12, 10, 11, 9, 23, 8, 22,
                                  20, 21, 19, 17, 18, 16};

void lights_key(uint8_t key, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *lv, bit[3];
    if (key >= BOARD_GUIDE_KEYS)
        return;
    if (key == 48) {                    /* the 25th key of chain B */
        lv = level_b, bit[0] = 78, bit[1] = 79, bit[2] = 77;
    } else {
        int local = key < 24 ? key : key - 24, g8 = local / 8, j = local % 8;
        lv = key < 24 ? level_a : level_b;
        for (int c = 0; c < 3; c++)
            bit[c] = (uint8_t)(24 * g8 + group[3 * j + c]);
    }
    lv[bit[0]] = r >> 1;
    lv[bit[1]] = g >> 1;
    lv[bit[2]] = b >> 1;
}

/* chain C bit + 1 of each control's light, 0 = none or not known (TODO: map on a keyboard) */
static const uint8_t ctl_bit[CTL_COUNT] = {0};

void lights_button(uint8_t ctl, uint8_t level)
{
    if (ctl < CTL_COUNT && ctl_bit[ctl])
        level_c[ctl_bit[ctl] - 1] = level >> 1;
}

void lights_strip(uint8_t strip, uint8_t style, uint16_t value)
{
    (void)strip, (void)style, (void)value;  /* TODO: the strip LEDs' chain C bits */
}

void lights_backlight(uint8_t level)
{
    backlight = level & 127;
}

static void port_on(uint32_t p, uint32_t cb, int buffered)
{
    xs1_setc(p, SETC_INUSE_ON);
    if (buffered) {
        xs1_setc(p, SETC_BUFFERS);
        xs1_settw(p, 8);
    }
    xs1_setclk(p, cb);
}

static void clock_out(uint32_t p, uint32_t cb)
{
    xs1_setc(p, SETC_INUSE_ON);
    xs1_setc(p, SETC_CLOCKPORT);
    xs1_setclk(p, cb);
}

/* a one-clock latch pulse at port time at: every port's high is queued before any low,
   since an output waits for the one before it and a time already past waits a full wrap */
static void latch_high(uint32_t p, uint32_t at)
{
    xs1_setpt(p, at & 0xFFFF);
    xs1_out(p, 1);
}

static void latch_low(uint32_t p, uint32_t at)
{
    xs1_setpt(p, (at + 1) & 0xFFFF);
    xs1_out(p, 0);
}

static void ab_thread(uint32_t arg)
{
    (void)arg;
    uint8_t a[10], b[10];
    xs1_clock_init(AB_CB, 32);          /* 100 MHz / 64 */
    port_on(A_DATA, AB_CB, 1);
    port_on(B_DATA, AB_CB, 1);
    port_on(A_LATCH, AB_CB, 0);
    port_on(B_LATCH, AB_CB, 0);
    clock_out(AB_CLK, AB_CB);
    xs1_setc(AB_CB, SETC_START);
    xs1_out(A_LATCH, 0);
    uint32_t at = xs1_getts(A_LATCH) + 200;
    for (;;)
        for (int k = 0; k < SHIFT595_PLANES; k++) {
            shift595_plane(level_a, 80, k, a);
            shift595_plane(level_b, 80, k, b);
            xs1_setpt(A_DATA, (at - 80) & 0xFFFF);
            xs1_setpt(B_DATA, (at - 80) & 0xFFFF);
            for (int j = 0; j < 10; j++) {
                xs1_out(A_DATA, a[j]);
                xs1_out(B_DATA, b[j]);
            }
            latch_high(A_LATCH, at);
            latch_high(B_LATCH, at);
            latch_low(A_LATCH, at);
            latch_low(B_LATCH, at);
            at += 122u << k;
        }
}

static void c_thread(uint32_t arg)
{
    (void)arg;
    uint8_t c[14];
    xs1_clock_init(C_CB, 39);           /* 100 MHz / 78 */
    port_on(C_DATA, C_CB, 1);
    port_on(C_LATCH, C_CB, 0);
    port_on(BACKLIGHT, C_CB, 0);
    clock_out(C_CLK, C_CB);
    xs1_setc(C_CB, SETC_START);
    xs1_out(C_LATCH, 0);
    uint32_t at = xs1_getts(C_LATCH) + 200;
    for (;;)
        for (int k = 0; k < SHIFT595_PLANES; k++) {
            shift595_plane(level_c, 112, k, c);
            xs1_setpt(C_DATA, (at - 112) & 0xFFFF);
            for (int j = 0; j < 14; j++)
                xs1_out(C_DATA, c[j]);
            latch_high(C_LATCH, at);
            xs1_setpt(BACKLIGHT, at & 0xFFFF);
            xs1_out(BACKLIGHT, backlight >> k & 1);
            latch_low(C_LATCH, at);
            at += 128u << k;
        }
}

void lights_start(void)
{
    xs1_thread_start(ab_thread, 0, ab_stack, sizeof ab_stack / 4);
    xs1_thread_start(c_thread, 0, c_stack, sizeof c_stack / 4);
}
