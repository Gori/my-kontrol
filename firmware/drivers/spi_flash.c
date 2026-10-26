/* SPI NOR flash commands (spi_flash.h). Our code, from the two chips' command sets. */
#include "spi_flash.h"

enum {
    CMD_WREN = 0x06, CMD_RDSR = 0x05, CMD_WRSR = 0x01, CMD_READ = 0x03, CMD_PP = 0x02,
    CMD_SE4K = 0x20, CMD_JEDEC = 0x9F,
};
#define SR_BUSY 0x01

static void cmd(const spi_bus_t *b, const uint8_t *c, int n, int keep)
{
    b->select(1);
    for (int i = 0; i < n; i++)
        b->xfer(c[i]);
    if (!keep)
        b->select(0);
}

static uint8_t status(const spi_bus_t *b)
{
    b->select(1);
    b->xfer(CMD_RDSR);
    uint8_t s = b->xfer(0);
    b->select(0);
    return s;
}

static void wait_ready(const spi_bus_t *b)
{
    for (uint32_t i = 0; i < 1000000 && (status(b) & SR_BUSY); i++)
        ;
}

static void write_status(const spi_bus_t *b, uint8_t v)
{
    const uint8_t wren = CMD_WREN, wrsr[2] = {CMD_WRSR, v};
    cmd(b, &wren, 1, 0);
    cmd(b, wrsr, 2, 0);
    wait_ready(b);
}

uint32_t spi_flash_jedec(const spi_bus_t *b)
{
    b->select(1);
    b->xfer(CMD_JEDEC);
    uint32_t id = (uint32_t)b->xfer(0) << 16;
    id |= (uint32_t)b->xfer(0) << 8;
    id |= b->xfer(0);
    b->select(0);
    return id;
}

int spi_flash_detect(const spi_bus_t *b)
{
    uint32_t id = spi_flash_jedec(b);
    return id == 0x1F4501 ? SPI_FLASH_AT25DF081A : id == 0xC22014 ? SPI_FLASH_MX25L8006E : SPI_FLASH_NONE;
}

void spi_flash_read(const spi_bus_t *b, uint32_t addr, uint8_t *buf, uint32_t n)
{
    const uint8_t c[4] = {CMD_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    cmd(b, c, 4, 1);
    for (uint32_t i = 0; i < n; i++)
        buf[i] = b->xfer(0);
    b->select(0);
}

/* the protection bits that block erase/program: AT25DF081A SWP (bits 2..3, cleared by a
   global unprotect, status 0; put back by a global protect, 0x7F), MX25L8006E BP0..2 (bits
   2..4, written back as they were) */
static int unprotect(const spi_bus_t *b, int chip, uint8_t *was)
{
    *was = status(b);
    uint8_t prot = chip == SPI_FLASH_AT25DF081A ? 0x0C : 0x1C;
    if (!(*was & prot))
        return 0;
    write_status(b, 0x00);
    return 1;
}

static void protect(const spi_bus_t *b, int chip, uint8_t was)
{
    write_status(b, chip == SPI_FLASH_AT25DF081A ? 0x7F : (uint8_t)(was & 0x9C));
}

static int allowed(int chip, uint32_t addr, uint32_t n, uint32_t lo, uint32_t hi)
{
    if (chip == SPI_FLASH_NONE)
        return -2;
    if (addr < lo || addr >= hi || n > hi - addr)
        return -1;
    return 0;
}

/* the image window must lie inside [SPI_FLASH_IMAGE_FLOOR, SPI_FLASH_IMAGE_CEIL) */
static int window(uint32_t *lo, uint32_t *hi)
{
    if (*lo == SPI_FLASH_FIRST_WRITABLE && *hi == SPI_FLASH_SIZE)
        return 0;
    return *lo < SPI_FLASH_IMAGE_FLOOR || *hi > SPI_FLASH_IMAGE_CEIL || *lo >= *hi ? -1 : 0;
}

static int erase_in(const spi_bus_t *b, int chip, uint32_t addr, uint32_t lo, uint32_t hi)
{
    addr &= ~0xFFFu;
    int r = window(&lo, &hi);
    if (!r)
        r = allowed(chip, addr, 4096, lo, hi);
    if (r)
        return r;
    uint8_t was;
    int restore = unprotect(b, chip, &was);
    const uint8_t wren = CMD_WREN, c[4] = {CMD_SE4K, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    cmd(b, &wren, 1, 0);
    cmd(b, c, 4, 0);
    wait_ready(b);
    if (restore)
        protect(b, chip, was);
    return 0;
}

static int write_in(const spi_bus_t *b, int chip, uint32_t addr, const uint8_t *p, uint32_t n,
                    uint32_t lo, uint32_t hi)
{
    int r = window(&lo, &hi);
    if (!r)
        r = allowed(chip, addr, n, lo, hi);
    if (r)
        return r;
    uint8_t was;
    int restore = unprotect(b, chip, &was);
    while (n) {
        uint32_t k = 256 - (addr & 255);           /* one page at a time */
        if (k > n)
            k = n;
        const uint8_t wren = CMD_WREN, c[4] = {CMD_PP, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
        cmd(b, &wren, 1, 0);
        cmd(b, c, 4, 1);
        for (uint32_t i = 0; i < k; i++)
            b->xfer(p[i]);
        b->select(0);
        wait_ready(b);
        addr += k, p += k, n -= k;
    }
    if (restore)
        protect(b, chip, was);
    return 0;
}

int spi_flash_erase(const spi_bus_t *b, int chip, uint32_t addr)
{
    return erase_in(b, chip, addr, SPI_FLASH_FIRST_WRITABLE, SPI_FLASH_SIZE);
}

int spi_flash_write(const spi_bus_t *b, int chip, uint32_t addr, const uint8_t *p, uint32_t n)
{
    return write_in(b, chip, addr, p, n, SPI_FLASH_FIRST_WRITABLE, SPI_FLASH_SIZE);
}

int spi_flash_erase_image(const spi_bus_t *b, int chip, uint32_t addr, uint32_t lo, uint32_t hi)
{
    if (lo == SPI_FLASH_FIRST_WRITABLE && hi == SPI_FLASH_SIZE)
        return -1;                                 /* not a way around the guard */
    return erase_in(b, chip, addr, lo, hi);
}

int spi_flash_write_image(const spi_bus_t *b, int chip, uint32_t addr, const uint8_t *p, uint32_t n,
                          uint32_t lo, uint32_t hi)
{
    if (lo == SPI_FLASH_FIRST_WRITABLE && hi == SPI_FLASH_SIZE)
        return -1;
    return write_in(b, chip, addr, p, n, lo, hi);
}
