/* Tile 0 test program for the S49 harness (tools/xcore/s49run.py): allocates the tile link,
   initialises the three LCD controllers as NI's firmware does, draws a test text on displays
   1 and 9, lights light-guide key 1 red and button light bit 3, waits for tile 1's USB_STATE, sends one DIN message and then echoes link input:
   DIN_RX -> DIN_TX, USB_MIDI_RX -> USB_MIDI_TX. Hardware per docs/hardware/s-mk1.md; LCD RAM layout per xmos.md §11.3. */
#include <stdint.h>
#include "xs1io.h"

#define PORT_4A 0x40000u            /* CS0..2 (active low), A0 */
#define PORT_4B 0x40100u            /* RST, SDA, SCK */

static uint32_t tmr, va = 0xf, vb;
static uint8_t lcd[3][240];

/* 16-segment cells (bit layout xmos.md §11.1) for the characters used here */
static uint16_t glyph(char c)
{
    switch (c) {
    case 'K': return 0x8CC0; case 'L': return 0x00F0; case 'M': return 0x05CC;
    case 'N': return 0x81CC; case 'O': return 0x00FF; case 'R': return 0x98C7;
    case 'S': return 0x8133; case 'T': return 0x4203; case 'X': return 0xA500;
    case 'Y': return 0x4500; case '1': return 0x4231; case '4': return 0x188C;
    case '9': return 0x18BF; default: return 0;
    }
}

/* row 1 / row 2 cell bit -> (byte offset from 5q, bit), §11.3 */
static const uint8_t row1[16][2] = {{1,7},{2,7},{4,7},{4,4},{3,4},{2,4},{1,4},{1,6},{2,6},{3,6},{3,7},{1,5},{4,6},{2,5},{3,5},{4,5}};
static const uint8_t row2[16][2] = {{2,3},{3,3},{4,2},{4,0},{3,0},{2,0},{0,0},{1,3},{1,2},{2,2},{3,2},{1,1},{4,1},{1,0},{2,1},{3,1}};
static const uint8_t row2_last[16][2] = {{0,3},{1,3},{3,3},{3,0},{2,0},{1,0},{0,0},{0,2},{1,2},{2,2},{2,3},{0,1},{3,2},{1,1},{2,1},{3,1}};

static void put_char(int display, int row, int pos, char c)
{
    uint8_t *buf = lcd[display / 3];
    int q = 8 * (display % 3) + pos;
    uint16_t cell = glyph(c);
    for (int b = 0; b < 16; b++) {
        if (!(cell >> b & 1))
            continue;
        const uint8_t *m = row == 1 ? row1[b] : pos < 7 ? row2[b] : row2_last[b];
        int off = 5 * q + m[0] + (row == 2 && pos == 7);
        buf[off] |= (uint8_t)(1 << m[1]);
    }
}

static void put_text(int display, int row, const char *s)
{
    for (int i = 0; i < 8 && s[i]; i++)
        put_char(display, row, i, s[i]);
}

static void lcd_byte(uint32_t cs_mask, int data, uint8_t b)
{
    va = (0x7 & ~cs_mask) | (data ? 8 : 0);
    out(PORT_4A, va);
    for (int i = 7; i >= 0; i--) {
        vb = (vb & ~6u) | ((b >> i & 1) << 1);
        out(PORT_4B, vb);
        out(PORT_4B, vb | 4);
    }
    out(PORT_4B, vb);
}

static void lcd_init(void)
{
    static const uint8_t init[] = {0xA3, 0xA0, 0xC0, 0x24, 0x81, 0x03, 0x2C, 0x2E, 0x2F, 0xAF, 0xA4};
    uint32_t t;
    setc(PORT_4A, SETC_INUSE_ON);
    setc(PORT_4B, SETC_INUSE_ON);
    out(PORT_4A, va);
    vb = 0;
    out(PORT_4B, vb);                        /* reset low 100 us */
    t = in(tmr);
    wait_until(tmr, t + 10000);
    vb = 1;
    out(PORT_4B, vb);
    wait_until(tmr, t + 110000);
    for (unsigned i = 0; i < sizeof init; i++)
        lcd_byte(7, 0, init[i]);
}

static void lcd_refresh(void)
{
    for (int c = 0; c < 3; c++)
        for (int half = 0; half < 2; half++) {
            const uint8_t cmd[] = {0x40, 0x00, half ? 0xBF : 0xB0, 0x12, 0x04};
            for (int i = 0; i < 5; i++)
                lcd_byte(1u << c, 0, cmd[i]);
            for (int i = 0; i < 120; i++)
                lcd_byte(1u << c, 1, lcd[c][120 * half + i]);
        }
}

/* shift a 74HC595 chain (data, latch: 1-bit ports) so that only buffer bit `on` is lit
   (outputs active low); bits go out last byte first, LSB first (xmos.md §8.3) */
static void chain_one(uint32_t data, uint32_t latch, int nbits, int on)
{
    setc(data, SETC_INUSE_ON);
    setc(latch, SETC_INUSE_ON);
    out(latch, 0);
    for (int k = 0; k < nbits; k++) {
        int idx = 8 * (nbits / 8 - 1 - k / 8) + k % 8;
        out(data, idx != on);
    }
    out(latch, 1);
    out(latch, 0);
}

int main(void)
{
    uint8_t p[256];
    uint32_t n;
    uint32_t link = xs1_chanend0;               /* chanend 0 (crt0.s) = LINK_TILE0 */
    setd(link, LINK_TILE1);
    tmr = getr(RES_TIMER);
    lcd_init();
    put_text(0, 1, "MY");
    put_text(0, 2, "KONTROL");
    put_text(8, 2, "XS1 S49");
    lcd_refresh();
    chain_one(0x10600, 0x10400, 72, 6);         /* 1G/1E: light guide key 1 red */
    chain_one(0x10c00, 0x10d00, 112, 3);        /* 1M/1N: button light bit 3 */
    while (link_recv(link, p, &n) != LINK_USB_STATE)
        ;
    static const uint8_t hello[] = {0xB0, 0x7B, 0x00};
    link_send(link, LINK_DIN_TX, hello, 3);
    for (;;) {
        uint32_t type = link_recv(link, p, &n);
        if (type == LINK_DIN_RX)
            link_send(link, LINK_DIN_TX, p, n);
        else if (type == LINK_USB_MIDI_RX)
            link_send(link, LINK_USB_MIDI_TX, p, n);
    }
}
