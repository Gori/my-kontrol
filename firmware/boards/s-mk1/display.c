/* The S MK1's 9 segment displays: 3 ST7565 controllers on a bit-banged bus (tile 0).
   4A: bits 0..2 chip select of controller 0..2 (active low), bit 3 A0 (1 = data).
   4B: bit 0 reset (active low), bit 1 data, bit 2 clock; data MSB first, clock 1 MHz.
   docs/hardware/s-mk1.md, Displays. */
#include <string.h>
#include "display.h"
#include "st7565_seg.h"
#include "xs1.h"

#define P4A PORT_4('A')
#define P4B PORT_4('B')
#define HALF_BIT 50                     /* ticks: 500 ns high, 500 ns low */

static uint32_t tmr, v4b = 1, t;
static st7565_seg_ram_t ram, sent;
static int have_sent;

static void bus_select(uint8_t ctrls, int data)
{
    xs1_out(P4A, (~ctrls & 7) | (data ? 8 : 0));
}

static void bus_write(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        v4b = (v4b & ~6u) | (b >> i & 1) << 1;
        xs1_out(P4B, v4b);
        xs1_wait_until(tmr, t += HALF_BIT);
        xs1_out(P4B, v4b | 4);
        xs1_wait_until(tmr, t += HALF_BIT);
    }
    xs1_out(P4B, v4b);
}

static void bus_reset(int active)
{
    v4b = active ? v4b & ~1u : v4b | 1;
    xs1_out(P4B, v4b);
}

static void bus_delay(uint32_t ms)
{
    xs1_wait_until(tmr, t = xs1_time(tmr) + ms * TICKS_PER_MS);
}

static const st7565_bus_t bus = { bus_select, bus_write, bus_reset, bus_delay };

void display_init(void)
{
    tmr = xs1_getr(RES_TIMER);
    t = xs1_time(tmr);
    xs1_setc(P4A, SETC_INUSE_ON);
    xs1_setc(P4B, SETC_INUSE_ON);
    xs1_out(P4A, 7);
    st7565_seg_init(&bus);
}

void display_show(const seg_display_t *d)
{
    st7565_seg_encode(ram, d);
    if (have_sent && !memcmp(ram, sent, sizeof ram))
        return;                         /* 6 ms on the bus: only when something changed */
    t = xs1_time(tmr);
    st7565_seg_send(&bus, (const uint8_t (*)[ST7565_SEG_BYTES])ram);
    memcpy(sent, ram, sizeof ram);
    have_sent = 1;
}
