#pragma once
/* A fake 1 MB SPI NOR flash for host tests (read, write enable, 4 KB erase, page program,
   status), laid out like the S49's boot flash, and XMOS images built like
   tools/xcore/ximage.py builds them (our own test bytes, not NI's). */
#include <stdint.h>
#include "spi_flash.h"

typedef struct {
    uint8_t mem[SPI_FLASH_SIZE];
    int cs, n, wel;
    uint8_t cmd[4];
    uint32_t addr;
    uint32_t bad_bit_at;             /* program this address with bit 0 stuck at 0 (0 = none) */
    int changes;                     /* erases and page programs that reached the chip */
} fake_flash_t;

extern fake_flash_t fake_flash;
extern const spi_bus_t fake_flash_bus;

/* an image with two segments of the given sizes into out (>= 256 KB); the padded size */
uint32_t fake_image(uint8_t *out, uint32_t seg0, uint32_t seg1, uint8_t fill, int rev);
/* boot loader (factory image at 0x1000, about 80 KB), NI's data at 0x4C000, the rest erased;
   an upgrade image at `up` unless 0 */
void fake_flash_layout(int rev, uint32_t up);
