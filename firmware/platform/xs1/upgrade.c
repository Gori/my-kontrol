/* The upgrade image slot of the XMOS boot flash (upgrade.h). */
#include <string.h>
#include "upgrade.h"

#define SECTOR 4096u

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

static uint32_t word_of(const uint8_t *p, int rev)
{
    uint32_t w = 0;
    for (int i = 3; i >= 0; i--)
        w = w << 8 | (rev ? rev8(p[i]) : p[i]);
    return w;
}

static uint32_t word_at(const spi_bus_t *b, uint32_t a, int rev)
{
    uint8_t p[4];
    spi_flash_read(b, a, p, 4);
    return word_of(p, rev);
}

/* XS1 crc32 step with polynomial 0xEDB88320 (tools/xcore/ximage.py) */
static uint32_t crc_step(uint32_t r, uint32_t x)
{
    for (int i = 0; i < 32; i++)
        r = r >> 1 ^ (r & 1 ? 0xEDB88320u : 0);
    return r ^ x;
}

/* image length from a header, 0 if it is not one */
static uint32_t header_len(const uint8_t *h, int rev, uint32_t max)
{
    uint32_t len = word_of(h + 0x0C, rev), n = word_of(h + 0x18, rev);
    if (word_of(h, rev) != UPGRADE_MAGIC || len != word_of(h + 0x14, rev) || len % 4 ||
        n < 1 || n > 8 || len < 0x1C + 12 * n || len > max)
        return 0;
    return len;
}

int upgrade_scan(const spi_bus_t *b, upgrade_slot_t *s)
{
    memset(s, 0, sizeof *s);
    for (int rev = 1; rev >= 0; rev--) {
        uint32_t n = word_at(b, 0, rev), f = 4 * n + 8;
        uint8_t h[0x1C];
        if (n == 0 || f >= SPI_FLASH_IMAGE_FLOOR)
            continue;
        spi_flash_read(b, f, h, sizeof h);
        uint32_t len = header_len(h, rev, 0x30000);
        if (!len)
            continue;
        s->rev = (uint8_t)rev;
        s->factory = f, s->factory_len = len;
        uint32_t end = (f + len + SECTOR - 1) & ~(SECTOR - 1);
        if (end < SPI_FLASH_IMAGE_FLOOR)
            end = SPI_FLASH_IMAGE_FLOOR;
        for (uint32_t a = end; a + SECTOR <= SPI_FLASH_IMAGE_CEIL; a += SECTOR) {
            spi_flash_read(b, a, h, sizeof h);
            uint32_t l = header_len(h, rev, UPGRADE_MAX);
            if (l) {
                s->image = a, s->len = l;
                break;
            }
        }
        s->lo = s->len ? s->image : (end + 0xFFFF) & ~0xFFFFu;
        s->hi = s->lo + UPGRADE_MAX;
        if (s->hi > SPI_FLASH_IMAGE_CEIL)
            s->hi = SPI_FLASH_IMAGE_CEIL;
        if (s->lo + SECTOR > s->hi)
            break;
        return UPGRADE_OK;
    }
    memset(s, 0, sizeof *s);
    return UPGRADE_ERR_TARGET;
}

int upgrade_begin(upgrade_writer_t *w, const spi_bus_t *b, int chip)
{
    w->bus = b, w->chip = chip, w->off = 0;
    w->err = upgrade_scan(b, &w->slot);
    if (!w->err && spi_flash_erase_image(b, chip, w->slot.lo, w->slot.lo, w->slot.hi))
        w->err = UPGRADE_ERR_ERASE;
    w->slot.image = w->slot.lo, w->slot.len = 0;
    return w->err;
}

static int program(upgrade_writer_t *w, uint32_t a, const uint8_t *p)
{
    upgrade_slot_t *s = &w->slot;
    uint8_t back[UPGRADE_PAGE];
    if (a % SECTOR == 0 && a != s->lo && spi_flash_erase_image(w->bus, w->chip, a, s->lo, s->hi))
        return UPGRADE_ERR_ERASE;
    if (spi_flash_write_image(w->bus, w->chip, a, p, UPGRADE_PAGE, s->lo, s->hi))
        return UPGRADE_ERR_WRITE;
    spi_flash_read(w->bus, a, back, UPGRADE_PAGE);
    return memcmp(back, p, UPGRADE_PAGE) ? UPGRADE_ERR_VERIFY : UPGRADE_OK;
}

/* a full page: the first is kept for the end, the others are written now */
static void page_done(upgrade_writer_t *w)
{
    uint32_t a = w->slot.lo + (w->off - 1) / UPGRADE_PAGE * UPGRADE_PAGE;
    if (a == w->slot.lo)
        memcpy(w->page0, w->page, UPGRADE_PAGE);
    else if (!w->err)
        w->err = program(w, a, w->page);
}

int upgrade_write(upgrade_writer_t *w, const uint8_t *p, uint32_t n)
{
    if (w->err)
        return w->err;
    if (n > w->slot.hi - w->slot.lo - w->off)
        return w->err = UPGRADE_ERR_ADDRESS;
    while (n && !w->err) {
        uint32_t k = UPGRADE_PAGE - w->off % UPGRADE_PAGE;
        if (k > n)
            k = n;
        memcpy(w->page + w->off % UPGRADE_PAGE, p, k);
        w->off += k, p += k, n -= k;
        if (w->off % UPGRADE_PAGE == 0)
            page_done(w);
    }
    return w->err;
}

int upgrade_end(upgrade_writer_t *w)
{
    if (w->err)
        return w->err;
    if (w->off < 0x34)
        return w->err = UPGRADE_ERR_FILE;
    if (w->off % UPGRADE_PAGE) {
        memset(w->page + w->off % UPGRADE_PAGE, 0xFF, UPGRADE_PAGE - w->off % UPGRADE_PAGE);
        page_done(w);
        if (w->err)
            return w->err;
    }
    int rev = w->slot.rev;
    uint32_t len = header_len(w->page0, rev, w->off);
    if (!len)
        return w->err = UPGRADE_ERR_FILE;
    /* the header CRC: words 0x10..len, page 0 from RAM, the rest from the flash */
    uint32_t r = 0xFFFFFFFFu;
    uint8_t buf[UPGRADE_PAGE];
    for (uint32_t a = 0x10; a < len; a += 4) {
        const uint8_t *p;
        if (a < UPGRADE_PAGE) {
            p = w->page0 + a;
        } else {
            if (a % UPGRADE_PAGE == 0 || a == 0x10)
                spi_flash_read(w->bus, w->slot.lo + a / UPGRADE_PAGE * UPGRADE_PAGE, buf, UPGRADE_PAGE);
            p = buf + a % UPGRADE_PAGE;
        }
        r = crc_step(r, word_of(p, rev));
    }
    if ((~crc_step(r, 0)) != word_of(w->page0 + 8, rev))
        return w->err = UPGRADE_ERR_FILE;
    if ((w->err = program(w, w->slot.lo, w->page0)) == UPGRADE_OK)
        w->slot.len = len;
    return w->err;
}

uint32_t upgrade_read(const spi_bus_t *b, const upgrade_slot_t *s, uint32_t off, uint8_t *buf, uint32_t n)
{
    uint32_t size = (s->len + UPGRADE_PAGE - 1) / UPGRADE_PAGE * UPGRADE_PAGE;
    if (!s->len || off >= size)
        return 0;
    if (n > size - off)
        n = size - off;
    spi_flash_read(b, s->image + off, buf, n);
    return n;
}

int upgrade_revert(const spi_bus_t *b, int chip)
{
    upgrade_slot_t s;
    int r = upgrade_scan(b, &s);
    if (r || !s.len)
        return r ? r : UPGRADE_OK;
    return spi_flash_erase_image(b, chip, s.image, s.lo, s.hi) ? UPGRADE_ERR_ERASE : UPGRADE_OK;
}
