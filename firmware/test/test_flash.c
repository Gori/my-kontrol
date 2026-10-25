/* Host test for drivers/spi_flash.c against a fake SPI flash: JEDEC detection, the write
   enable before every change, status polling, the protection clear and restore, and the
   guard that keeps everything below 0x55000 (NI's data) untouched. */
#include <stdio.h>
#include <string.h>
#include "spi_flash.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- the fake chip ---------- */

static struct {
    uint32_t jedec;
    uint8_t mem[SPI_FLASH_SIZE];
    uint8_t sr, wel, busy_polls;
    int cs, n;
    uint8_t cmd[8];
    uint32_t addr;
    int refused;                       /* an erase/program the chip itself rejected */
    int low_writes;                    /* any change below 0x55000: must stay 0 */
    char log[512];
} chip;

static uint8_t prot_mask(void)
{
    return chip.jedec == 0x1F4501 ? 0x0C : 0x1C;
}

static void log_cmd(uint8_t c)
{
    char t[8];
    snprintf(t, sizeof t, "%02X ", c);
    strncat(chip.log, t, sizeof chip.log - strlen(chip.log) - 1);
}

static void fake_select(int on)
{
    if (on) {
        chip.cs = 1, chip.n = 0;
        return;
    }
    if (!chip.cs)
        return;
    chip.cs = 0;
    uint8_t c = chip.cmd[0];
    if (chip.n < 1)
        return;
    if (c == 0x06) {
        chip.wel = 1;
    } else if (c == 0x01 && chip.n >= 2) {
        if (chip.wel) {
            uint8_t v = chip.cmd[1];
            chip.sr = chip.jedec == 0x1F4501 ? (v == 0x7F ? 0x0C : v == 0 ? 0 : chip.sr) : (v & 0x9C);
            chip.busy_polls = 2;
        }
        chip.wel = 0;
    } else if (c == 0x20 && chip.n >= 4) {
        uint32_t a = (uint32_t)chip.cmd[1] << 16 | chip.cmd[2] << 8 | chip.cmd[3];
        if (!chip.wel || (chip.sr & prot_mask())) {
            chip.refused++;
        } else {
            if (a < SPI_FLASH_FIRST_WRITABLE)
                chip.low_writes++;
            memset(chip.mem + (a & ~0xFFFu), 0xFF, 4096);
            chip.busy_polls = 3;
        }
        chip.wel = 0;
    } else if (c == 0x02) {
        chip.wel = 0;
        chip.busy_polls = 2;
    }
}

static uint8_t fake_xfer(uint8_t out)
{
    if (!chip.cs)
        return 0xFF;
    int k = chip.n++;
    if (k < 8)
        chip.cmd[k] = out;
    if (k == 0)
        log_cmd(out);
    uint8_t c = chip.cmd[0];
    if (c == 0x9F && k >= 1 && k <= 3)
        return (uint8_t)(chip.jedec >> (8 * (3 - k)));
    if (c == 0x05 && k >= 1) {
        uint8_t s = chip.sr | (chip.wel ? 2 : 0) | (chip.busy_polls ? 1 : 0);
        if (chip.busy_polls)
            chip.busy_polls--;
        return s;
    }
    if (k == 3 && (c == 0x03 || c == 0x02))
        chip.addr = (uint32_t)chip.cmd[1] << 16 | chip.cmd[2] << 8 | out;
    if (c == 0x03 && k >= 4)
        return chip.mem[(chip.addr + k - 4) % SPI_FLASH_SIZE];
    if (c == 0x02 && k >= 4) {
        if (!chip.wel || (chip.sr & prot_mask())) {
            if (k == 4)
                chip.refused++;
        } else {
            uint32_t a = (chip.addr & ~0xFFu) | ((chip.addr + k - 4) & 0xFF);
            if (a < SPI_FLASH_FIRST_WRITABLE)
                chip.low_writes++;
            chip.mem[a] &= out;
        }
    }
    return 0xFF;
}

static const spi_bus_t bus = { fake_select, fake_xfer };

static void power_on(uint32_t jedec, uint8_t sr)
{
    memset(&chip, 0, sizeof chip);
    memset(chip.mem, 0xFF, sizeof chip.mem);
    memset(chip.mem, 0xA5, SPI_FLASH_FIRST_WRITABLE);   /* NI's things */
    chip.jedec = jedec;
    chip.sr = sr;
}

/* ---------- tests ---------- */

static void test_detect(void)
{
    power_on(0x1F4501, 0);
    CHECK(spi_flash_detect(&bus) == SPI_FLASH_AT25DF081A, "AT25DF081A");
    power_on(0xC22014, 0);
    CHECK(spi_flash_detect(&bus) == SPI_FLASH_MX25L8006E, "MX25L8006E");
    power_on(0xEF4014, 0);
    CHECK(spi_flash_detect(&bus) == SPI_FLASH_NONE, "an unknown chip");
    CHECK(spi_flash_write(&bus, SPI_FLASH_NONE, 0xF0000, (const uint8_t *)"x", 1) == -2,
          "no writes to an unknown chip");
}

static void test_write_protected(int jedec, int chip_kind, uint8_t sr)
{
    power_on((uint32_t)jedec, sr);
    uint8_t data[300], back[300];
    for (int i = 0; i < 300; i++)
        data[i] = (uint8_t)(i * 7);
    CHECK(spi_flash_erase(&bus, chip_kind, 0xF0123) == 0, "erase");
    CHECK(chip.refused == 0, "the chip accepted the erase (protection cleared)");
    CHECK((chip.sr & prot_mask()) == (sr & prot_mask()), "protection put back after the erase (sr %02x)", chip.sr);
    chip.log[0] = 0;
    CHECK(spi_flash_write(&bus, chip_kind, 0xF00F0, data, 300) == 0, "write across pages");
    CHECK(chip.refused == 0, "the chip accepted every page");
    spi_flash_read(&bus, 0xF00F0, back, 300);
    CHECK(!memcmp(back, data, 300), "read back");
    CHECK((chip.sr & prot_mask()) == (sr & prot_mask()), "protection put back after the write");
    /* sequence: status, unprotect (WREN WRSR), then WREN PP per page, polling, re-protect */
    if (sr & prot_mask())
        CHECK(!strncmp(chip.log, "05 06 01 05 ", 12), "status, write enable, clear protection: %s", chip.log);
    int pp = 0;
    for (char *p = chip.log; (p = strstr(p, "06 02 ")); p += 6)
        pp++;
    CHECK(pp == 3, "write enable before each of 3 pages (%d): %s", pp, chip.log);
}

static void test_guard(void)
{
    power_on(0x1F4501, 0x0C);
    uint8_t x = 0;
    CHECK(spi_flash_erase(&bus, SPI_FLASH_AT25DF081A, 0x4C000) == -1, "NI identity: refused");
    CHECK(spi_flash_erase(&bus, SPI_FLASH_AT25DF081A, 0x54FFF) == -1, "last NI sector: refused");
    CHECK(spi_flash_write(&bus, SPI_FLASH_AT25DF081A, 0x0, &x, 1) == -1, "boot loader: refused");
    CHECK(spi_flash_write(&bus, SPI_FLASH_AT25DF081A, 0x54FFF, &x, 2) == -1, "a write ending at 0x55000: refused");
    CHECK(spi_flash_write(&bus, SPI_FLASH_AT25DF081A, 0xFFFFF, &x, 2) == -1, "past the end: refused");
    CHECK(chip.log[0] == 0, "nothing reached the chip: %s", chip.log);
    CHECK(spi_flash_erase(&bus, SPI_FLASH_AT25DF081A, 0x55000) == 0, "0x55000 is ours");
    CHECK(chip.low_writes == 0 && chip.mem[0x54FFF] == 0xA5, "NI's data untouched");
}

int main(void)
{
#define RUN(t) do { current = #t; t; } while (0)
    RUN(test_detect());
    RUN(test_write_protected(0x1F4501, SPI_FLASH_AT25DF081A, 0x0C));   /* as NI leaves it */
    RUN(test_write_protected(0x1F4501, SPI_FLASH_AT25DF081A, 0x00));
    RUN(test_write_protected(0xC22014, SPI_FLASH_MX25L8006E, 0x1C));
    RUN(test_guard());
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("test_flash: all passed\n");
    return 0;
}
