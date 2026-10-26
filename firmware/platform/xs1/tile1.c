/* XS1 tile 1: USB, the 5-pin MIDI UART and the SPI flash, serving tile 0 over the tile link
   (link.h). Its own image (rules.mk), with crt0.s, rt.c, link.c and thread.c.

   Normal start, 5 threads:
   - main: the link. Packets from tile 0 (DIN bytes to the TX thread, flash requests done
     here, USB-MIDI and keyboard taps to the USB thread, update mode / factory requests),
     bytes from the RX thread (sent on as DIN_RX), and what the USB thread has for tile 0
     (USB-MIDI from the host, the USB state), whichever comes first.
   - DIN TX: 31 250 Bd 8N1 on 1J with timed port outputs; 1J is inverted like NI's firmware
     (the board's line driver inverts back).
   - DIN RX: 1L, waits for the start bit, samples each bit in its middle by port time.
   - USB: TinyUSB with our descriptors (usb/usb_link.c), the DFU runtime (usb/dfu_xmos.c).
   - SIE: the ULPI link to the PHY (usb_ulpi.c; HARDWARE-UNVERIFIED, see usb.md).
   - the flash (drivers/spi_flash.c): AT25DF081A or MX25L8006E by JEDEC ID; nothing below
     0x55000 (NI's data) is ever erased or written except the upgrade image slot in update
     mode (upgrade.c); protection bits cleared around changes.

   Update mode: RESETINTODFU (or DETACH) on USB, or LINK_SYS_UPDATE from tile 0, sets a word
   in RAM that a chip reset keeps and reboots. The next start sees it and runs only the SIE
   and the USB thread with the DFU device (PID +2), sends tile 0 one USB_STATE with the DFU
   bit and never waits for tile 0. The word is at the address where NI's firmware keeps its
   own DFU flag (fw notes xmos.md section 9), a place known to survive the reset and the boot
   loader; the value is ours. It lies above tile 1's image and bss (about 0x15700 now) and
   15 KB below the top of the main stack. Ports per docs/hardware/s-mk1.md (tile 1). */
#include <stdint.h>
#include "link.h"
#include "spi_flash.h"
#include "tile1_dfu.h"
#include "upgrade.h"
#include "usb_link.h"
#include "usb_ulpi.h"
#include "xs1.h"

#define DIN_OUT PORT_1('J')
#define DIN_IN  PORT_1('L')
#define SPI_SS   PORT_1('A')
#define SPI_CLK  PORT_1('B')
#define SPI_MISO PORT_1('C')
#define SPI_MOSI PORT_1('D')
#define BIT_TICKS 3200                  /* 31 250 Bd */
#define DFU_FLAG ((volatile uint32_t *)0x1C3B4)
#define DFU_MAGIC 0x4D4B4446u            /* "MKDF" */

static uint32_t tx_stack[148], rx_stack[148], sie_stack[160], usb_stack[320];   /* 512 free (xtest) */

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

/* ---------- reboot, USB ---------- */

static void reboot(int into_dfu)
{
    *DFU_FLAG = into_dfu ? DFU_MAGIC : 0;
    xs1_chip_reset();
}

static uint8_t chip_id[12];
static uint32_t note_tx;
static volatile uint8_t note_pending;

/* the USB thread has something for main: one token at most in flight */
void usb_link_notify(void)
{
    if (note_tx && !note_pending) {     /* none in update mode: no link side */
        note_pending = 1;
        xs1_outt(note_tx, 0);
    }
}

static void usb_loop(uint32_t dcd, int dfu)
{
    usb_dcd_bind_channel(dcd);
    usb_link_init(dfu, chip_id, tile1_dfu_backend(&spi, flash_chip, reboot));
    uint32_t tmr = xs1_getr(RES_TIMER);
    for (;;) {
        if (xs1_wait_chan_until(dcd, tmr, xs1_time(tmr) + TICKS_PER_MS) == 0)
            usb_dcd_pump();
        usb_link_task();
    }
}

static void usb_thread(uint32_t dcd)
{
    usb_loop(dcd, 0);
}

/* the SIE and the DCD: a chanend each, connected; returns the DCD's */
static uint32_t usb_start(void)
{
    uint32_t sie = xs1_getr(RES_CHANEND), dcd = xs1_getr(RES_CHANEND);
    xs1_setd(sie, dcd);
    xs1_setd(dcd, sie);
    xs1_thread_start(usb_ulpi_sie_thread, sie, sie_stack, sizeof sie_stack / 4);
    return dcd;
}

/* ---------- the link ---------- */

static void to_tile0(uint32_t link, uint8_t *p)
{
    static uint8_t mounted = 0xFF;
    uint32_t v, n = 0;
    note_pending = 0;
    if (usb_link_mounted != mounted) {
        mounted = usb_link_mounted;
        p[0] = mounted ? LINK_USB_MOUNTED : 0;
        link_send(link, LINK_USB_STATE, p, 1);
    }
    while (n + 4 <= LINK_MAX && usb_ring_get(&usb_midi_from_host, &v)) {
        p[n++] = (uint8_t)v, p[n++] = (uint8_t)(v >> 8), p[n++] = (uint8_t)(v >> 16), p[n++] = (uint8_t)(v >> 24);
        if (n + 4 > LINK_MAX) {
            link_send(link, LINK_USB_MIDI_RX, p, n);
            n = 0;
        }
    }
    if (n)
        link_send(link, LINK_USB_MIDI_RX, p, n);
#ifdef MK_DEV
    while (usb_ring_get(&usb_dev_events, &v)) {
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
        link_send(link, LINK_DEV_EVENT, p, 4);
    }
#endif
}

static void system_request(uint32_t link, uint8_t cmd)
{
    if (cmd == LINK_SYS_UPDATE)
        reboot(1);
    if (cmd == LINK_SYS_FACTORY) {
        uint8_t st = upgrade_revert(&spi, flash_chip) ? 1 : 0;
        link_send(link, LINK_FLASH_DONE, &st, 1);
        if (!st)
            reboot(0);
    }
}

int main(void)
{
    static uint8_t p[LINK_MAX + 1];
    uint32_t n;
    uint32_t link = xs1_chanend0;             /* chanend 0 (crt0.s) = LINK_TILE1 */
    uint32_t tmr = xs1_getr(RES_TIMER);
    xs1_setd(link, LINK_TILE0);
    int dfu = *DFU_FLAG == DFU_MAGIC;
    *DFU_FLAG = 0;
    spi_init();
    spi_flash_read(&spi, LINK_ID_FLASH, p, 64);
    link_chip_id(p, chip_id);
    xs1_wait_until(tmr, xs1_time(tmr) + TICKS_PER_MS);   /* tile 0 has its link end by now */

    if (dfu) {                                /* update mode: tile 0 is told once, then left alone */
        p[0] = LINK_USB_DFU;
        link_send(link, LINK_USB_STATE, p, 1);
        usb_loop(usb_start(), 1);
    }

    uint32_t tx_main = xs1_getr(RES_CHANEND), tx_end = xs1_getr(RES_CHANEND);
    uint32_t rx_main = xs1_getr(RES_CHANEND), rx_end = xs1_getr(RES_CHANEND);
    uint32_t note_rx = xs1_getr(RES_CHANEND);
    note_tx = xs1_getr(RES_CHANEND);
    xs1_setd(tx_main, tx_end);
    xs1_setd(tx_end, tx_main);
    xs1_setd(rx_main, rx_end);
    xs1_setd(rx_end, rx_main);
    xs1_setd(note_tx, note_rx);
    xs1_setd(note_rx, note_tx);
    xs1_thread_start(din_tx, tx_end, tx_stack, sizeof tx_stack / 4);
    xs1_thread_start(din_rx, rx_end, rx_stack, sizeof rx_stack / 4);
    p[0] = 0;
    link_send(link, LINK_USB_STATE, p, 1);
    xs1_thread_start(usb_thread, usb_start(), usb_stack, sizeof usb_stack / 4);

    for (;;) {
        int w = xs1_wait3(link, rx_main, note_rx);
        if (w == 1) {
            n = 0;
            do                                  /* what the RX thread has, in one packet */
                p[n++] = (uint8_t)xs1_int(rx_main);
            while (n < 32 && xs1_ready(rx_main));
            link_send(link, LINK_DIN_RX, p, n);
            continue;
        }
        if (w == 2) {
            (void)xs1_int(note_rx);
            to_tile0(link, p);
            continue;
        }
        uint8_t type = link_recv(link, p, &n);
        if (type == LINK_DIN_TX) {
            for (uint32_t i = 0; i < n; i++)
                xs1_outt(tx_main, p[i]);
        } else if (type == LINK_FLASH_READ || type == LINK_FLASH_ERASE || type == LINK_FLASH_WRITE) {
            flash_request(link, type, p, n);
        } else if (type == LINK_USB_MIDI_TX) {
            for (uint32_t i = 0; i + 4 <= n; i += 4)    /* dropped when the queue is full */
                usb_ring_put(&usb_midi_to_host, p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24);
        } else if (type == LINK_KBD_TAP && n >= 2) {
            usb_ring_put(&usb_kbd_taps, p[0] | p[1] << 8);
        } else if (type == LINK_SYSTEM && n >= 1) {
            system_request(link, p[0]);
        }
    }
}
