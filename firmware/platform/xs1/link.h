#pragma once
/* The tile link: tile 0 (core, panel) <-> tile 1 (USB, 5-pin MIDI, flash).

   - Each tile allocates its link chanend first in main, so it is chanend 0 of its node:
     LINK_TILE0 / LINK_TILE1 below (NI's node numbering: tile 0 = node 0x8000, tile 1 =
     node 0x0000). Each setd's its end to the other's.
   - Tile 1 speaks first: USB_STATE once it is up, at least 1 ms after boot, so tile 0 has
     allocated its end (tokens sent to an unallocated chanend are lost). Tile 0 sends
     nothing before it has received a packet.
   - A message is one packet of byte tokens [type, n, n payload bytes] and the END control
     token, which closes the route. n <= 255; numbers little endian.
   tools/xcore/s49emu.py (HostLink) implements tile 1's side for bring-up without USB. */
#include <stdint.h>

#define LINK_TILE0 0x80000002u
#define LINK_TILE1 0x00000002u
#define LINK_MAX   255

enum {
    /* tile 0 -> tile 1 */
    LINK_USB_MIDI_TX = 0x81,   /* 4-byte USB-MIDI event packets */
    LINK_DIN_TX = 0x82,        /* bytes for the 5-pin MIDI OUT */
    LINK_FLASH_READ = 0x83,    /* u32 address, u8 length       -> FLASH_DATA */
    LINK_FLASH_ERASE = 0x84,   /* u32 address (4 KB sector)    -> FLASH_DONE */
    LINK_FLASH_WRITE = 0x85,   /* u32 address, data (one page) -> FLASH_DONE */
    /* tile 1 -> tile 0 */
    LINK_USB_STATE = 0x01,     /* u8 1 = configured by a host; first, then on changes */
    LINK_USB_MIDI_RX = 0x02,   /* 4-byte USB-MIDI event packets from the host */
    LINK_DIN_RX = 0x03,        /* bytes from the 5-pin MIDI IN */
    LINK_FLASH_DATA = 0x04,
    LINK_FLASH_DONE = 0x05,    /* u8 0 = ok */
    LINK_DEV_EVENT = 0x06,     /* developer builds: u8 type, u8 id, s16 value -> app_event()
                                  (USB vendor request 0xB8 on the M32, tools/mktest.py) */
};

/* both tiles (platform/xs1/link.c) */
void link_send(uint32_t c, uint8_t type, const uint8_t *p, uint32_t n);
uint8_t link_recv(uint32_t c, uint8_t *p, uint32_t *n);   /* blocks for one packet */

/* tile 0 (tile0_link.c): the raw SPI flash, by flash address; tile 1 refuses any change
   below 0x55000 (NI's data). 0 = ok. */
void tile0_flash_read(uint32_t spi, uint8_t *buf, uint32_t n);
int tile0_flash_erase(uint32_t spi);
int tile0_flash_write(uint32_t spi, const uint8_t *s, uint32_t len);
