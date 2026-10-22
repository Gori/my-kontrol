/* 128x32 monochrome framebuffer in SSD1306 page layout. */
#include "gfx.h"
#include "display.h"
#include "fonts.h"

#define W 128
#define H 32

const font_t font_small = { 5, 8, 32, 95, spleen5x8_data };
const font_t font_large = { 8, 16, 32, 95, spleen8x16_data };

static uint8_t fb[W * H / 8];

void gfx_clear(void)
{
    for (unsigned i = 0; i < sizeof fb; i++)
        fb[i] = 0;
}

void gfx_pixel(int x, int y, int on)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return;
    uint8_t *p = &fb[(y >> 3) * W + x];
    if (on)
        *p |= 1 << (y & 7);
    else
        *p &= ~(1 << (y & 7));
}

void gfx_fill(int x, int y, int w, int h, int on)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            gfx_pixel(i, j, on);
}

void gfx_invert(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H)
                fb[(j >> 3) * W + i] ^= 1 << (j & 7);
}

int gfx_text(int x, int y, const char *s, const font_t *f)
{
    for (; *s; s++, x += f->w) {
        unsigned c = (uint8_t)*s - f->first;
        if (c >= f->count)
            c = '?' - f->first;
        const uint8_t *g = &f->data[c * f->h];
        for (int row = 0; row < f->h; row++)
            for (int col = 0; col < f->w; col++)
                if (g[row] & (0x80 >> col))
                    gfx_pixel(x + col, y + row, 1);
    }
    return x;
}

int gfx_text_width(const char *s, const font_t *f)
{
    int n = 0;
    while (s[n])
        n++;
    return n * f->w;
}

void gfx_text_center(int y, const char *s, const font_t *f)
{
    gfx_text((W - gfx_text_width(s, f)) / 2, y, s, f);
}

void gfx_flush(void)
{
    display_blit(fb);
}
