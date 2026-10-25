/* XS1 tile 1: USB, the 5-pin MIDI UART and the SPI flash, serving tile 0 over the tile link
   (link.h). Its own image (rules.mk), with crt0.s, rt.c, link.c and thread.c.

   Threads:
   - main: the link. Packets from tile 0 (DIN bytes to the TX thread, flash requests done
     here) and bytes from the RX thread (sent on as DIN_RX), whichever comes first.
   - DIN TX: 31 250 Bd 8N1 on 1J with timed port outputs; 1J is inverted like NI's firmware
     (the board's line driver inverts back).
   - DIN RX: 1L, waits for the start bit, samples each bit in its middle by port time.
   - the flash (drivers/spi_flash.c): AT25DF081A or MX25L8006E by JEDEC ID; nothing below
     0x55000 (NI's data) is ever erased or written; protection bits cleared around changes.
   USB is a stub so far: no enumeration, USB_STATE 0 (no host). Ports per
   docs/hardware/s-mk1.md (tile 1). */
#include <stdint.h>
#include "link.h"
#include "spi_flash.h"
#include "xs1.h"

#define DIN_OUT PORT_1('J')
#define DIN_IN  PORT_1('L')
#define SPI_SS   PORT_1('A')
#define SPI_CLK  PORT_1('B')
#define SPI_MISO PORT_1('C')
#define SPI_MOSI PORT_1('D')
#define BIT_TICKS 3200                  /* 31 250 Bd */

static uint32_t tx_stack[64], rx_stack[64];

/* ---------- 5-pin MIDI ---------- */

static void din_tx(uint32_t c)          /* c: chanend, one byte token per MIDI byte */
{
    xs1_setc(DIN_OUT, SETC_INUSE_ON);
    xs1_setc(DIN_OUT, SETC_INVERT);
    xs1_out(DIN_OUT, 1);
    for (;;) {
        uint32_t frame = xs1_int(c) << 1 | 0x200;
        xs1_out(DIN_OUT, 1);
        uint32_t t = xs1_getts(DIN_OUT);
        for (int i = 0; i < 10; i++) {
            t += BIT_TICKS;
            xs1_setpt(DIN_OUT, t);
            xs1_out(DIN_OUT, frame >> i & 1);
        }
    }
}

static void din_rx(uint32_t c)          /* c: chanend to main, one token per byte received */
{
    xs1_setc(DIN_IN, SETC_INUSE_ON);
    for (;;) {
        xs1_setd(DIN_IN, 0);
        xs1_setc(DIN_IN, SETC_COND_EQ);    /* the start bit */
        (void)xs1_in(DIN_IN);
        uint32_t t = xs1_getts(DIN_IN) + BIT_TICKS / 2, b = 0;
        xs1_setc(DIN_IN, SETC_COND_FULL);
        for (int i = 0; i < 9; i++) {
            t += BIT_TICKS;
            xs1_setpt(DIN_IN, t);
            uint32_t v = xs1_in(DIN_IN) & 1;
            if (i < 8)
                b |= v << i;
            else if (v)                    /* a stop bit: a byte */
                xs1_outt(c, b);
        }
    }
}

/* ---------- SPI flash (drivers/spi_flash.c; mode 0, MSB first, bit-banged) ---------- */

static uint8_t spi_byte(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        xs1_out(SPI_MOSI, out >> i & 1);
        xs1_out(SPI_CLK, 1);
        in = in << 1 | (xs1_in(SPI_MISO) & 1);
        xs1_out(SPI_CLK, 0);
    }
    return in;
}

static void spi_select(int on)
{
    xs1_out(SPI_SS, !on);
}

static const spi_bus_t spi = { spi_select, spi_byte };
static int flash_chip;

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* a FLASH_* request from tile 0; erases and writes below 0x55000 are refused by the driver
   and answered with status 1 */
static void flash_request(uint32_t link, uint8_t type, uint8_t *p, uint32_t n)
{
    uint32_t a = get32(p);
    if (type == LINK_FLASH_READ) {
        uint32_t len = p[4];
        spi_flash_read(&spi, a, p, len);
        link_send(link, LINK_FLASH_DATA, p, len);
        return;
    }
    int r = type == LINK_FLASH_ERASE ? spi_flash_erase(&spi, flash_chip, a)
                                     : spi_flash_write(&spi, flash_chip, a, p + 4, n - 4);
    uint8_t st = r ? 1 : 0;
    link_send(link, LINK_FLASH_DONE, &st, 1);
}

static void spi_init(void)
{
    xs1_setc(SPI_SS, SETC_INUSE_ON);
    xs1_setc(SPI_CLK, SETC_INUSE_ON);
    xs1_setc(SPI_MISO, SETC_INUSE_ON);
    xs1_setc(SPI_MOSI, SETC_INUSE_ON);
    xs1_out(SPI_SS, 1);
    xs1_out(SPI_CLK, 0);
    flash_chip = spi_flash_detect(&spi);
}

/* ---------- the link ---------- */

int main(void)
{
    static uint8_t p[LINK_MAX + 1];
    uint32_t n;
    uint32_t link = xs1_getr(RES_CHANEND);    /* first: chanend 0 = LINK_TILE1 */
    uint32_t tmr = xs1_getr(RES_TIMER);
    uint32_t tx_main = xs1_getr(RES_CHANEND), tx_end = xs1_getr(RES_CHANEND);
    uint32_t rx_main = xs1_getr(RES_CHANEND), rx_end = xs1_getr(RES_CHANEND);
    xs1_setd(link, LINK_TILE0);
    xs1_setd(tx_main, tx_end);
    xs1_setd(tx_end, tx_main);
    xs1_setd(rx_main, rx_end);
    xs1_setd(rx_end, rx_main);
    spi_init();
    xs1_thread_start(din_tx, tx_end, tx_stack, sizeof tx_stack / 4);
    xs1_thread_start(din_rx, rx_end, rx_stack, sizeof rx_stack / 4);

    xs1_wait_until(tmr, xs1_time(tmr) + TICKS_PER_MS);   /* tile 0 has its link end by now */
    uint8_t usb = 0;
    link_send(link, LINK_USB_STATE, &usb, 1);

    for (;;) {
        if (xs1_wait2(link, rx_main) == 1) {
            n = 0;
            do                                  /* what the RX thread has, in one packet */
                p[n++] = (uint8_t)xs1_int(rx_main);
            while (n < 32 && xs1_ready(rx_main));
            link_send(link, LINK_DIN_RX, p, n);
            continue;
        }
        uint8_t type = link_recv(link, p, &n);
        if (type == LINK_DIN_TX) {
            for (uint32_t i = 0; i < n; i++)
                xs1_outt(tx_main, p[i]);
        } else if (type == LINK_FLASH_READ || type == LINK_FLASH_ERASE || type == LINK_FLASH_WRITE) {
            flash_request(link, type, p, n);
        }
    }
}
