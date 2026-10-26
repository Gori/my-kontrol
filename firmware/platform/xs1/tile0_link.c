/* XS1 tile 0: the core's USB and 5-pin MIDI over the tile link (link.h), in place of
   usb/usb_device.c; TinyUSB runs on tile 1 (usb/usb_link.c). Also the settings flash (read,
   erased and written on tile 1's SPI flash, by request) and update mode.

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

void xs1_link_up(void);
void xs1_update_mode(void) __attribute__((noreturn));

static uint32_t link;
static uint8_t up, mounted;
static uint8_t pkt[LINK_MAX];
static uint32_t din_budget_us, din_last_ms;

/* called first in main, before any other chanend is allocated */
void tile0_link_open(void)
{
    link = xs1_chanend0;            /* chanend 0: LINK_TILE0 */
    xs1_setd(link, LINK_TILE1);
}

static void handle(uint8_t type, const uint8_t *p, uint32_t n)
{
    switch (type) {
    case LINK_USB_STATE:
        up = 1;
        if (n && (p[0] & LINK_USB_DFU))
            xs1_update_mode();      /* tile 1 is in update mode: stop here */
        if (n && (p[0] & LINK_USB_MOUNTED) != mounted) {
            mounted = p[0] & LINK_USB_MOUNTED;
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

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v, b[1] = v >> 8, b[2] = v >> 16, b[3] = v >> 24;
}

/* raw SPI flash through tile 1 (link.h); tile 1 refuses changes below 0x55000 */
void tile0_flash_read(uint32_t spi, uint8_t *buf, uint32_t n)
{
    while (n) {
        uint32_t k = n < 128 ? n : 128;
        uint8_t q[5];
        put32(q, spi);
        q[4] = (uint8_t)k;
        request(LINK_FLASH_READ, q, 5, LINK_FLASH_DATA, buf, k);
        spi += k, buf += k, n -= k;
    }
}

int tile0_flash_erase(uint32_t spi)
{
    uint8_t q[4], r = 1;
    put32(q, spi);
    request(LINK_FLASH_ERASE, q, 4, LINK_FLASH_DONE, &r, 1);
    return r;
}

int tile0_flash_write(uint32_t spi, const uint8_t *s, uint32_t len)
{
    while (len) {
        uint32_t n = 256 - (spi & 255);                /* within one SPI page */
        if (n > 200) n = 200;
        if (n > len) n = len;
        uint8_t q[4 + 200], r = 1;
        put32(q, spi);
        memcpy(q + 4, s, n);
        request(LINK_FLASH_WRITE, q, 4 + n, LINK_FLASH_DONE, &r, 1);
        if (r)
            return r;
        spi += n, s += n, len -= n;
    }
    return 0;
}

/* the settings store's flash (BOARD_FLASH_READ): SPI flash addresses, read by copy */
void hal_flash_read(uintptr_t addr, void *dst, uint32_t n)
{
    tile0_flash_read(addr, dst, n);
}

int hal_flash_erase(uintptr_t addr)
{
    return tile0_flash_erase(addr & ~0xFFFu);
}

int hal_flash_write(uintptr_t addr, const void *src, uint32_t len)
{
    return tile0_flash_write(addr, src, len);
}

void tile0_link_wait(void)
{
    uint32_t n;
    while (!up) {
        uint8_t type = link_recv(link, pkt, &n);
        handle(type, pkt, n);
    }
}

int tile0_link_is_up(void)
{
    return up;
}

/* LINK_SYSTEM: update mode (tile 1 reboots, no answer) or the factory image (status) */
int tile0_system(uint8_t cmd)
{
    uint8_t r = 1;
    if (cmd == LINK_SYS_FACTORY)
        request(LINK_SYSTEM, &cmd, 1, LINK_FLASH_DONE, &r, 1);
    else
        link_send(link, LINK_SYSTEM, &cmd, 1);
    return r;
}

void usb_init(void)
{
    tile0_link_wait();
    xs1_link_up();                  /* hal.c: the crash counter (may stop here), the ID */
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
    uint8_t p[2] = {modifier, key};
    if (mounted)
        link_send(link, LINK_KBD_TAP, p, 2);
}
