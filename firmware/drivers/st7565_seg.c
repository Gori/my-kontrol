/* ST7565 segment-glass driver (st7565_seg.h). Our code, from the documented wiring. */
#include "st7565_seg.h"
#include "font16.h"

/* Where a character's 16 segments sit. Display slot s (0..2) of a controller, character
   p (0..7): q = 8s + p, the character's RAM starts at byte 5q of page A (RAM bytes 0..119).
   Each entry is (byte offset, bit) for segment bit 0..15. */
static const uint8_t line1_map[16][2] = {
    {1, 7}, {2, 7}, {4, 7}, {4, 4}, {3, 4}, {2, 4}, {1, 4}, {1, 6},
    {2, 6}, {3, 6}, {3, 7}, {1, 5}, {4, 6}, {2, 5}, {3, 5}, {4, 5},
};
static const uint8_t line2_map[16][2] = {
    {2, 3}, {3, 3}, {4, 2}, {4, 0}, {3, 0}, {2, 0}, {0, 0}, {1, 3},
    {1, 2}, {2, 2}, {3, 2}, {1, 1}, {4, 1}, {1, 0}, {2, 1}, {3, 1},
};
/* the last character of line 2 is wired differently; offsets from 5q + 1 */
static const uint8_t line2_last_map[16][2] = {
    {0, 3}, {1, 3}, {3, 3}, {3, 0}, {2, 0}, {1, 0}, {0, 0}, {0, 2},
    {1, 2}, {2, 2}, {2, 3}, {0, 1}, {3, 2}, {1, 1}, {2, 1}, {3, 1},
};
/* Row 0 of a display (docs/hardware/s-mk1.md, tested): the meter is 9 positions, each a
   block and a thin line, in a box. Page-B byte offsets (RAM 120 + 40s + n, bit 0) of the
   blocks and lines of positions 1..9, and of the box. */
static const uint8_t meter_block[9] = {7, 11, 4, 1, 24, 26, 29, 36, 32};
static const uint8_t meter_line[9] = {6, 9, 12, 2, 39, 27, 37, 34, 31};
#define METER_BOX 14

static void set(uint8_t *ram, int byte, int bit)
{
    ram[byte] |= (uint8_t)(1u << bit);
}

static void encode_display(uint8_t *ram, int s, const seg_display_t *d)
{
    int page_b = 120 + 40 * s;
    for (int p = 0; p < 8; p++) {
        int base = 5 * (8 * s + p);
        uint16_t c1 = font16(d->line[0][p]), c2 = font16(d->line[1][p]);
        for (int b = 0; b < 16; b++) {
            if (c1 >> b & 1)
                set(ram, base + line1_map[b][0], line1_map[b][1]);
            if (c2 >> b & 1) {
                if (p < 7)
                    set(ram, base + line2_map[b][0], line2_map[b][1]);
                else
                    set(ram, base + 1 + line2_last_map[b][0], line2_last_map[b][1]);
            }
        }
        if (p < 7 && d->dots[0] >> p & 1)       /* no dot after the 8th character */
            set(ram, base + 4, 3);
        if (p < 6 && d->dots[1] >> p & 1)
            set(ram, base + 5, 1);
        if (p == 6 && d->dots[1] >> p & 1)
            set(ram, base + 5, 0);
    }
    if (d->meter) {                             /* the box, the line, n blocks */
        int n = (d->meter * 9 + 254) / 255;     /* 1..255 shows at least one */
        set(ram, page_b + METER_BOX, 0);
        for (int i = 0; i < 9; i++) {
            set(ram, page_b + meter_line[i], 0);
            if (i < n)
                set(ram, page_b + meter_block[i], 0);
        }
    }
}

void st7565_seg_encode(st7565_seg_ram_t ram, const seg_display_t d[9])
{
    for (int c = 0; c < ST7565_SEG_CTRLS; c++)
        for (int i = 0; i < ST7565_SEG_BYTES; i++)
            ram[c][i] = 0;
    for (int n = 0; n < 9; n++)
        encode_display(ram[n / 3], n % 3, &d[n]);
}

static void command(const st7565_bus_t *bus, uint8_t ctrls, const uint8_t *b, int n)
{
    bus->select(ctrls, 0);
    for (int i = 0; i < n; i++)
        bus->write(b[i]);
}

void st7565_seg_init(const st7565_bus_t *bus)
{
    /* bias 1/7, ADC normal, COM normal, regulator 4, contrast 3, then the power
       circuits one step at a time, display on, normal (not all-on) */
    static const uint8_t setup[] = {0xA3, 0xA0, 0xC0, 0x24, 0x81, 0x03, 0x2C};
    static const uint8_t on[] = {0xAF, 0xA4};
    bus->reset(1);
    bus->delay_ms(1);
    bus->reset(0);
    bus->delay_ms(1);
    command(bus, 7, setup, sizeof setup);
    bus->delay_ms(1);
    command(bus, 7, (const uint8_t[]){0x2E}, 1);
    bus->delay_ms(1);
    command(bus, 7, (const uint8_t[]){0x2F}, 1);
    bus->delay_ms(1);
    command(bus, 7, on, sizeof on);
}

void st7565_seg_send(const st7565_bus_t *bus, const st7565_seg_ram_t ram)
{
    for (int c = 0; c < ST7565_SEG_CTRLS; c++)
        for (int half = 0; half < 2; half++) {
            /* start line 0, page 0 (A) or 15 (B), column 0x24 */
            const uint8_t addr[] = {0x40, 0x00, half ? 0xBF : 0xB0, 0x12, 0x04};
            command(bus, 1 << c, addr, sizeof addr);
            bus->select(1 << c, 1);
            for (int i = 0; i < 120; i++)
                bus->write(ram[c][120 * half + i]);
        }
}
