/* XS1 tile 0: the core's USB and 5-pin MIDI over the tile link (link.h), in place of
   usb/usb_device.c and TinyUSB, which run on tile 1. Also the settings flash, a RAM mirror of
   the SPI flash pages that tile 1 reads and writes.

   usb_task() (main loop) takes what tile 1 sent: USB_STATE -> app_usb_mounted(),
   USB_MIDI_RX -> midi_rx_packet(), DIN_RX -> midi_din_rx_byte(); and sends the core's
   queues: USB-MIDI packets while a host is connected, 5-pin bytes at MIDI speed (tile 1's
   UART buffer stays short). */
#include <string.h>
#include "app.h"
#include "hal.h"
#include "kbd.h"
#include "link.h"
#include "midi.h"
#include "usb.h"
#include "xs1.h"

static uint32_t link;
static uint8_t up, mounted;
static uint8_t pkt[LINK_MAX];
static uint32_t din_budget_us, din_last_ms;

uint8_t xs1_flash_mirror[4 * FLASH_PAGE];

/* called first in main, before any other chanend is allocated */
void tile0_link_open(void)
{
    link = xs1_getr(RES_CHANEND);
    xs1_setd(link, LINK_TILE1);
}

static void handle(uint8_t type, const uint8_t *p, uint32_t n)
{
    switch (type) {
    case LINK_USB_STATE:
        up = 1;
        if (n && (p[0] & 1) != mounted) {
            mounted = p[0] & 1;
            app_usb_mounted(mounted);
        }
        break;
    case LINK_USB_MIDI_RX:          /* a packet the EXT queue refuses is dropped (TODO: hold) */
        for (uint32_t i = 0; i + 4 <= n; i += 4)
            midi_rx_packet(p + i);
        break;
#ifdef MK_DEV
    case LINK_DEV_EVENT:
        if (n >= 4) {
            mk_event_t ev = {p[0], p[1], (int16_t)(p[2] | p[3] << 8)};
            app_event(&ev);
        }
        break;
#endif
#if BOARD_MIDI_DIN
    case LINK_DIN_RX:
        for (uint32_t i = 0; i < n; i++)
            midi_din_rx_byte(p[i]);
        break;
#endif
    }
}

static void poll(void)
{
    uint32_t n;
    for (int k = 0; k < 4 && xs1_ready(link); k++) {
        uint8_t type = link_recv(link, pkt, &n);
        handle(type, pkt, n);
    }
}

/* a request to tile 1 and its answer; other packets meanwhile are handled as usual */
static int request(uint8_t type, const uint8_t *p, uint32_t n, uint8_t want, uint8_t *out, uint32_t max)
{
    uint32_t got;
    link_send(link, type, p, n);
    for (;;) {
        uint8_t t = link_recv(link, pkt, &got);
        if (t == want) {
            if (out)
                memcpy(out, pkt, got < max ? got : max);
            return (int)got;
        }
        handle(t, pkt, got);
    }
}

static uint32_t spi_addr(uintptr_t a)
{
    uintptr_t off = a - (uintptr_t)xs1_flash_mirror;
    return BOARD_SPI_SETTINGS + off / FLASH_PAGE * 4096 + off % FLASH_PAGE;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v, b[1] = v >> 8, b[2] = v >> 16, b[3] = v >> 24;
}

static void flash_load(void)
{
    for (uint32_t off = 0; off < sizeof xs1_flash_mirror; off += 128) {
        uint8_t q[5];
        put32(q, spi_addr((uintptr_t)xs1_flash_mirror + off));
        q[4] = 128;
        request(LINK_FLASH_READ, q, 5, LINK_FLASH_DATA, xs1_flash_mirror + off, 128);
    }
}

int hal_flash_erase(uintptr_t addr)
{
    uint8_t q[4], r = 1;
    uintptr_t page = addr & ~(uintptr_t)(FLASH_PAGE - 1);
    put32(q, spi_addr(page));
    request(LINK_FLASH_ERASE, q, 4, LINK_FLASH_DONE, &r, 1);
    memset((void *)page, 0xFF, FLASH_PAGE);
    return r;
}

int hal_flash_write(uintptr_t addr, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint32_t a = spi_addr(addr), n = 256 - (a & 255);   /* within one SPI page */
        if (n > 200) n = 200;
        if (n > len) n = len;
        uint8_t q[4 + 200], r = 1;
        put32(q, a);
        memcpy(q + 4, s, n);
        request(LINK_FLASH_WRITE, q, 4 + n, LINK_FLASH_DONE, &r, 1);
        if (r)
            return r;
        for (uint32_t i = 0; i < n; i++)
            ((uint8_t *)addr)[i] &= s[i];
        addr += n, s += n, len -= n;
    }
    return 0;
}

void usb_init(void)
{
    uint32_t n;
    while (!up) {                   /* tile 1 is up when it says USB_STATE */
        uint8_t type = link_recv(link, pkt, &n);
        handle(type, pkt, n);
    }
    flash_load();
    din_last_ms = hal_ms();
}

void usb_task(void)
{
    uint8_t out[4 * 16];
    uint32_t n = 0;
    poll();
    while (n + 4 <= sizeof out && midi_tx_peek(out + n)) {
        midi_tx_pop();
        if (mounted)
            n += 4;
    }
    if (n)
        link_send(link, LINK_USB_MIDI_TX, out, n);
#if BOARD_MIDI_DIN
    uint32_t now = hal_ms();
    din_budget_us += (now - din_last_ms) * 1000;   /* 320 us per byte at 31 250 Bd */
    din_last_ms = now;
    if (din_budget_us > 20000)
        din_budget_us = 20000;
    n = 0;
    int b;
    while (n < 16 && din_budget_us >= 320 && (b = midi_din_tx_byte()) >= 0) {
        out[n++] = (uint8_t)b;
        din_budget_us -= 320;
    }
    if (n)
        link_send(link, LINK_DIN_TX, out, n);
#endif
}

/* main loop (HAL_HAS_IDLE): wait for a packet from tile 1 or the next millisecond */
void hal_idle(void)
{
    static uint32_t tmr;
    if (!tmr)
        tmr = xs1_getr(RES_TIMER);
    uint32_t now = xs1_time(tmr);
    xs1_wait_chan_until(link, tmr, now + TICKS_PER_MS - now % TICKS_PER_MS);
}

void kbd_tap(uint8_t modifier, uint8_t key)
{
    (void)modifier, (void)key;      /* no USB keyboard until tile 1 runs USB */
}
