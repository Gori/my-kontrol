#pragma once
/* SPI NOR flash of the S MK1 (docs/hardware/s-mk1.md, Flash): AT25DF081A (JEDEC 1F 45 01)
   or MX25L8006E (C2 20 14), 1 MB, 4 KB erase sectors, 256-byte pages.

   Guards:
   - erase and write refuse anything below SPI_FLASH_FIRST_WRITABLE: the boot loader, NI's
     factory and upgrade images and NI's identity, settings and template data live there,
     and must stay untouched so NI's firmware gives back the keyboard as it was;
   - the status register's protection bits (NI leaves them set) are cleared before an erase
     or a write and put back afterwards.
   The board passes the bus: chip select and one byte each way. Host-tested (test_flash.c). */
#include <stdint.h>

#define SPI_FLASH_SIZE           0x100000u
#define SPI_FLASH_FIRST_WRITABLE 0x55000u

enum { SPI_FLASH_NONE, SPI_FLASH_AT25DF081A, SPI_FLASH_MX25L8006E };

typedef struct {
    void (*select)(int on);           /* chip select (active low on the wire) */
    uint8_t (*xfer)(uint8_t out);     /* one byte out (MSB first) and one in */
} spi_bus_t;

int spi_flash_detect(const spi_bus_t *bus);                 /* SPI_FLASH_*, from the JEDEC ID */
uint32_t spi_flash_jedec(const spi_bus_t *bus);
void spi_flash_read(const spi_bus_t *bus, uint32_t addr, uint8_t *buf, uint32_t n);
/* 0 = done; -1 = refused (protected range or out of the chip), -2 = no or unknown chip */
int spi_flash_erase(const spi_bus_t *bus, int chip, uint32_t addr);        /* the 4 KB sector */
int spi_flash_write(const spi_bus_t *bus, int chip, uint32_t addr, const uint8_t *p, uint32_t n);
