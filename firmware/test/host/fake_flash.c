/* A fake SPI flash and XMOS test images (fake_flash.h). */
#include <string.h>
#include "fake_flash.h"

#define UPGRADE_MAGIC 0x01A551E5u

/* ---------- a fake SPI flash: read, WREN, 4K erase, page program, status ---------- */

fake_flash_t fake_flash;
#define f fake_flash

static void fsel(int on)
{
    if (on) {
        f.cs = 1, f.n = 0;
        return;
    }
    if (f.cs && f.n >= 1) {
        if (f.cmd[0] == 0x06)
            f.wel = 1;
        else if (f.cmd[0] == 0x20 && f.n >= 4 && f.wel) {
            uint32_t a = (f.cmd[1] << 16 | f.cmd[2] << 8 | f.cmd[3]) & ~0xFFFu;
            memset(f.mem + a, 0xFF, 4096);
            f.changes++;
            f.wel = 0;
        } else if (f.cmd[0] == 0x02 || f.cmd[0] == 0x01)
            f.wel = 0;
    }
    f.cs = 0;
}

static uint8_t fxfer(uint8_t out)
{
    int k = f.n++;
    if (k < 4)
        f.cmd[k] = out;
    uint8_t c = f.cmd[0];
    if (k == 3 && (c == 0x03 || c == 0x02))
        f.addr = (uint32_t)f.cmd[1] << 16 | f.cmd[2] << 8 | out;
    if (c == 0x9F && k >= 1 && k <= 3)
        return (uint8_t)(0x1F4501 >> (8 * (3 - k)));
    if (c == 0x05 && k >= 1)
        return f.wel ? 2 : 0;
    if (c == 0x03 && k >= 4)
        return f.mem[(f.addr + k - 4) % SPI_FLASH_SIZE];
    if (c == 0x02 && k >= 4 && f.wel) {
        uint32_t a = (f.addr & ~0xFFu) | ((f.addr + k - 4) & 0xFF);
        f.mem[a] &= out;
        if (a == f.bad_bit_at && f.bad_bit_at)
            f.mem[a] &= 0xFE;
        if (k == 4)
            f.changes++;
    }
    return 0xFF;
}

const spi_bus_t fake_flash_bus = { fsel, fxfer };

/* ---------- images, as xflash stores them ---------- */

static uint8_t rev8(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        r |= ((b >> i) & 1) << (7 - i);
    return r;
}

static uint32_t step(uint32_t r, uint32_t x)
{
    for (int i = 0; i < 32; i++)
        r = r >> 1 ^ (r & 1 ? 0xEDB88320u : 0);
    return r ^ x;
}

static void put(uint8_t *p, uint32_t v)
{
    p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24;
}

static uint32_t get(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* an image with two segments of the given sizes; returns the padded size */
uint32_t fake_image(uint8_t *out, uint32_t seg0, uint32_t seg1, uint8_t fill, int rev)
{
    uint32_t segs[2] = {seg0, seg1}, off = 0x1C + 24;
    memset(out, 0, 0x40000);
    for (int k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < segs[k]; i++)
            out[off + i] = (uint8_t)(fill + i * 13 + k);
        uint32_t r = step(0xFFFFFFFFu, segs[k] / 4);
        for (uint32_t i = 0; i < segs[k]; i += 4)
            r = step(r, get(out + off + i));
        put(out + off + segs[k], ~step(r, 0));
        put(out + 0x1C + 12 * k, off);
        put(out + 0x20 + 12 * k, k ? 2 : 0x80000002u);
        put(out + 0x24 + 12 * k, segs[k] / 4 + 1);
        off += segs[k] + 4;
    }
    uint32_t len = off;
    put(out + 0x00, UPGRADE_MAGIC);
    put(out + 0x04, 0x12345678);
    put(out + 0x0C, len);
    put(out + 0x10, 0);
    put(out + 0x14, len);
    put(out + 0x18, 2);
    uint32_t r = 0xFFFFFFFFu;
    for (uint32_t a = 0x10; a < len; a += 4)
        r = step(r, get(out + a));
    put(out + 0x08, ~step(r, 0));
    uint32_t size = (len + 255) & ~255u;
    if (rev)
        for (uint32_t i = 0; i < size; i++)
            out[i] = rev8(out[i]);
    return size;
}

static uint8_t img[0x40000];

void fake_flash_layout(int rev, uint32_t up)
{
    memset(&f, 0, sizeof f);
    memset(f.mem, 0xFF, sizeof f.mem);
    uint32_t n = 0x3FE;                                   /* factory at 4n + 8 = 0x1000 */
    uint8_t w[4];
    put(w, n);
    for (int i = 0; i < 4; i++)
        f.mem[i] = rev ? rev8(w[i]) : w[i];
    for (uint32_t i = 4; i < 4 * n + 8; i++)
        f.mem[i] = (uint8_t)(i * 3);                      /* "loader" */
    uint32_t size = fake_image(img, 0xB000, 0x9000, 0x11, rev);   /* factory: ends 0x15100 */
    memcpy(f.mem + 4 * n + 8, img, size);
    for (uint32_t a = 0x4C000; a < 0x55000; a++)
        f.mem[a] = (uint8_t)(a * 7);                      /* "NI's data" */
    if (up) {
        size = fake_image(img, 0x6000, 0x4000, 0x55, rev);
        memcpy(f.mem + up, img, size);
    }
}
