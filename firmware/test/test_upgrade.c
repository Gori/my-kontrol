/* Host test for platform/xs1/upgrade.c (the DFU target: the upgrade image slot of the XMOS
   boot flash) on a fake SPI flash laid out like the S49's: a boot loader, a factory image,
   maybe an upgrade image, NI's data at 0x4C000. Images are built here like
   tools/xcore/ximage.py builds them (our own test bytes). Checks where the slot is, that a
   download is written, verified and read back, that nothing outside the slot changes, and
   that every failure leaves no valid upgrade image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upgrade.h"
#include "fake_flash.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define f fake_flash
#define bus fake_flash_bus
#define make_image fake_image
#define flash_layout fake_flash_layout

static uint8_t img[0x40000], back[0x40000], snap[SPI_FLASH_SIZE];

/* everything outside [lo, hi) is as in snap */
static int outside_same(uint32_t lo, uint32_t hi)
{
    return !memcmp(f.mem, snap, lo) && !memcmp(f.mem + hi, snap + hi, SPI_FLASH_SIZE - hi);
}

static int download(const uint8_t *p, uint32_t n, int end)
{
    static upgrade_writer_t w;
    int r = upgrade_begin(&w, &bus, SPI_FLASH_AT25DF081A);
    for (uint32_t o = 0; !r && o < n; o += 64)
        r = upgrade_write(&w, p + o, n - o < 64 ? n - o : 64);
    if (!r && end)
        r = upgrade_end(&w);
    return r;
}

/* ---------- tests ---------- */

static void test_slot(void)
{
    upgrade_slot_t s;
    flash_layout(1, 0);
    CHECK(upgrade_scan(&bus, &s) == 0, "scan");
    CHECK(s.rev == 1 && s.factory == 0x1000, "factory at 4n + 8 (rev %d, 0x%x)", s.rev, s.factory);
    CHECK(s.len == 0 && s.lo == 0x20000 && s.hi == 0x40000, "no upgrade image: slot at the next 64 KB, 128 KB (0x%x-0x%x)", s.lo, s.hi);
    flash_layout(1, 0x16000);
    CHECK(upgrade_scan(&bus, &s) == 0 && s.image == 0x16000 && s.lo == 0x16000 && s.len > 0,
          "an upgrade image is found where it is (0x%x, %u bytes)", s.image, s.len);
    CHECK(s.hi == 0x36000, "slot end 0x%x", s.hi);
    flash_layout(0, 0x16000);
    CHECK(upgrade_scan(&bus, &s) == 0 && s.rev == 0 && s.image == 0x16000, "plain words are recognised too");
    memset(f.mem, 0xFF, sizeof f.mem);
    CHECK(upgrade_scan(&bus, &s) == UPGRADE_ERR_TARGET, "an erased flash: no slot");
}

static void test_download(int rev, uint32_t existing)
{
    flash_layout(rev, existing);
    memcpy(snap, f.mem, sizeof snap);
    uint32_t size = make_image(img, 0xC000, 0x8000, 0x77, rev);
    upgrade_slot_t s0;
    upgrade_scan(&bus, &s0);
    CHECK(download(img, size, 1) == 0, "download of %u bytes", size);
    upgrade_slot_t s;
    CHECK(upgrade_scan(&bus, &s) == 0 && s.image == s0.lo && s.len, "the new image is found at 0x%x", s.image);
    CHECK(!memcmp(f.mem + s.image, img, size), "flash holds the image");
    CHECK(outside_same(s0.lo, s0.hi), "nothing outside the slot 0x%x-0x%x changed", s0.lo, s0.hi);
    uint32_t n = 0, k;
    while ((k = upgrade_read(&bus, &s, n, back + n, 64)))
        n += k;
    CHECK(n == size && !memcmp(back, img, size), "upload gives the image back (%u of %u bytes)", n, size);
}

static void test_failures(void)
{
    upgrade_slot_t s;
    uint32_t size;

    flash_layout(1, 0x16000);
    size = make_image(img, 0xC000, 0x8000, 0x77, 1);
    CHECK(download(img, size, 0) == 0, "first part");
    CHECK(upgrade_scan(&bus, &s) == 0 && s.len == 0, "cut off: no valid upgrade image, the factory image boots");

    flash_layout(1, 0x16000);
    img[0x5000] ^= 0x10;
    CHECK(download(img, size, 1) == UPGRADE_ERR_FILE, "a corrupted image is refused");
    CHECK(upgrade_scan(&bus, &s) == 0 && s.len == 0, "and leaves no valid upgrade image");

    flash_layout(1, 0);
    memset(img, 0x5A, 0x8000);
    CHECK(download(img, 0x8000, 1) == UPGRADE_ERR_FILE, "not an image");

    flash_layout(1, 0);
    size = make_image(img, 0x14000, 0x10000, 0x33, 1);
    CHECK(download(img, size, 1) == UPGRADE_ERR_ADDRESS, "too big for the slot (%u bytes)", size);

    flash_layout(1, 0);
    size = make_image(img, 0xC000, 0x8000, 0x77, 1);
    f.bad_bit_at = 0x20000 + 0x3001;                  /* a page that programs wrong */
    img[0x3001] |= 1;
    CHECK(download(img, size, 1) == UPGRADE_ERR_VERIFY, "read-back after programming catches a bad page");
    CHECK(upgrade_scan(&bus, &s) == 0 && s.len == 0, "and leaves no valid upgrade image");

    memset(&f, 0, sizeof f);
    memset(f.mem, 0xFF, sizeof f.mem);
    size = make_image(img, 0xC000, 0x8000, 0x77, 1);
    CHECK(download(img, size, 1) == UPGRADE_ERR_TARGET && f.changes == 0,
          "no factory image found: nothing is written (%d changes)", f.changes);
}

static void test_revert(void)
{
    upgrade_slot_t s;
    flash_layout(1, 0x16000);
    memcpy(snap, f.mem, sizeof snap);
    CHECK(upgrade_revert(&bus, SPI_FLASH_AT25DF081A) == 0, "revert");
    CHECK(upgrade_scan(&bus, &s) == 0 && s.len == 0, "no upgrade image after a revert");
    CHECK(outside_same(0x16000, 0x17000), "only the image's first sector changed");
}

static void test_window(void)
{
    flash_layout(1, 0);
    memcpy(snap, f.mem, sizeof snap);
    uint8_t x[256];
    memset(x, 0, sizeof x);
    int c = SPI_FLASH_AT25DF081A;
    CHECK(spi_flash_erase_image(&bus, c, 0x1000, 0x1000, 0x20000) == -1, "window below the floor (factory)");
    CHECK(spi_flash_write_image(&bus, c, 0x0, x, 4, 0x0, 0x20000) == -1, "the boot loader");
    CHECK(spi_flash_erase_image(&bus, c, 0x4C000, 0x20000, 0x50000) == -1, "window into NI's data");
    CHECK(spi_flash_write_image(&bus, c, 0x1F000, x, 256, 0x20000, 0x40000) == -1, "below the window");
    CHECK(spi_flash_write_image(&bus, c, 0x3FF80, x, 256, 0x20000, 0x40000) == -1, "across its end");
    CHECK(spi_flash_erase_image(&bus, c, 0xF0000, SPI_FLASH_FIRST_WRITABLE, SPI_FLASH_SIZE) == -1,
          "not a second way to the settings area");
    CHECK(spi_flash_erase(&bus, c, 0x20000) == -1, "the normal guard still refuses the slot");
    CHECK(f.changes == 0 && !memcmp(f.mem, snap, sizeof snap), "nothing reached the chip");
}

int main(void)
{
#define RUN(t) do { current = #t; t; } while (0)
    RUN(test_slot());
    RUN(test_download(1, 0));
    RUN(test_download(1, 0x16000));
    RUN(test_download(0, 0));
    RUN(test_failures());
    RUN(test_revert());
    RUN(test_window());
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("test_upgrade: all passed\n");
    return 0;
}
