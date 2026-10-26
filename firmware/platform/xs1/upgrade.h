#pragma once
/* The upgrade image slot of the XMOS boot flash (docs/hardware/s-mk1.md, Flash; fw notes
   xmos.md sections 3 and 13), for DFU on tile 1:

     0x0              boot loader, word 0 = its size n in words
     4n + 8           the factory image (NI firmware 0x14): never written
     after it         the upgrade image slot, at most 128 KB, below NI's data at 0x4C000
     0x4C000-0x54FFF  NI's identity, settings, templates: never written

   Image words are little endian after reversing the bits of every byte (xflash stores them
   so); the scan also accepts plain words, and uses whichever finds the factory image's magic.
   Without a factory image nothing is written. The slot is where an upgrade image already is
   (the boot loader found it there), else the first 64 KB boundary after the factory image
   (a sector start for 4 KB and 64 KB sector layouts alike).

   Writing (upgrade_begin / _write / _end): the slot's first sector is erased first, so the
   old image stops being valid at once; every page is read back after programming; the first
   page (the header, with the magic) is held in RAM and written last, only after the whole
   image's header CRC checks out. A download that is cut off or fails leaves no valid upgrade
   image, and the boot loader starts the factory image. Host-tested (test/test_upgrade.c). */
#include <stdint.h>
#include "spi_flash.h"

#define UPGRADE_MAGIC   0x01A551E5u
#define UPGRADE_MAX     0x20000u
#define UPGRADE_PAGE    256u

/* errors, as DFU status codes (DFU 1.1 table 6.2) */
enum {
    UPGRADE_OK = 0,
    UPGRADE_ERR_TARGET = 1,    /* no factory image found: the layout is not what we expect */
    UPGRADE_ERR_FILE = 2,      /* not an image, or its CRC is wrong */
    UPGRADE_ERR_WRITE = 3,
    UPGRADE_ERR_ERASE = 4,
    UPGRADE_ERR_VERIFY = 7,
    UPGRADE_ERR_ADDRESS = 8,   /* bigger than the slot */
    UPGRADE_ERR_NOTDONE = 9,
};

typedef struct {
    uint32_t factory, factory_len;
    uint32_t lo, hi;          /* the slot */
    uint32_t image, len;      /* the upgrade image in the slot; len 0 = none */
    uint8_t rev;              /* words bit-reversed per byte */
} upgrade_slot_t;

typedef struct {
    const spi_bus_t *bus;
    int chip;
    upgrade_slot_t slot;
    uint32_t off;             /* bytes received */
    int err;
    uint8_t page[UPGRADE_PAGE], page0[UPGRADE_PAGE];
} upgrade_writer_t;

int upgrade_scan(const spi_bus_t *bus, upgrade_slot_t *s);          /* 0 or UPGRADE_ERR_TARGET */
int upgrade_begin(upgrade_writer_t *w, const spi_bus_t *bus, int chip);
int upgrade_write(upgrade_writer_t *w, const uint8_t *p, uint32_t n);
int upgrade_end(upgrade_writer_t *w);
/* the installed upgrade image, padded to a page: bytes at off (0 past the end) */
uint32_t upgrade_read(const spi_bus_t *bus, const upgrade_slot_t *s, uint32_t off, uint8_t *buf, uint32_t n);
/* erase the upgrade image's first sector: the boot loader then starts the factory image */
int upgrade_revert(const spi_bus_t *bus, int chip);
