#include "gfx.h"
#include "hal.h"
#include "fonts.h"

#define W DISP_W
#define H DISP_H

const font_t font_small = {5, 8, 32, 95, spleen5x8_data};
const font_t font_large = {8, 16, 32, 95, spleen8x16_data};
const font_t font_huge = {12, 24, 32, 95, spleen12x24_data};

uint8_t gfx_fb[W * H / 8];

void gfx_clear(void)
{
    for (unsigned i = 0; i < sizeof gfx_fb; i++)
        gfx_fb[i] = 0;
}

void gfx_pixel(int x, int y, int on)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return;
    uint8_t *p = &gfx_fb[(y >> 3) * W + x];
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

void gfx_frame(int x, int y, int w, int h)
{
    gfx_fill(x, y, w, 1, 1);
    gfx_fill(x, y + h - 1, w, 1, 1);
    gfx_fill(x, y, 1, h, 1);
    gfx_fill(x + w - 1, y, 1, h, 1);
}

void gfx_hline_dotted(int x, int y, int w)
{
    for (int i = 0; i < w; i += 2)
        gfx_pixel(x + i, y, 1);
}

void gfx_invert(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H)
                gfx_fb[(j >> 3) * W + i] ^= 1 << (j & 7);
}

static void glyph(int x, int y, char ch, const font_t *f)
{
    unsigned c = (uint8_t)ch - f->first;
    if (c >= f->count)
        c = '?' - f->first;
    int bpr = (f->w + 7) / 8;
    const uint8_t *g = &f->data[c * f->h * bpr];
    for (int row = 0; row < f->h; row++, g += bpr)
        for (int col = 0; col < f->w; col++)
            if (g[col >> 3] & (0x80 >> (col & 7)))
                gfx_pixel(x + col, y + row, 1);
}

int gfx_text_n(int x, int y, const char *s, int max_chars, const font_t *f)
{
    for (; *s && max_chars-- > 0; s++, x += f->w)
        glyph(x, y, *s, f);
    return x;
}

int gfx_text(int x, int y, const char *s, const font_t *f)
{
    return gfx_text_n(x, y, s, 255, f);
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
    int w = gfx_text_width(s, f);
    gfx_text(w > W ? 0 : (W - w) / 2, y, s, f);
}

void gfx_text_right(int right, int y, const char *s, const font_t *f)
{
    gfx_text(right - gfx_text_width(s, f), y, s, f);
}

/* Horizontal value bar: outline plus fill; bipolar fills from the centre. */
void gfx_bar(int x, int y, int w, int h, int value, int max, int bipolar)
{
    if (max <= 0)
        max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    gfx_frame(x, y, w, h);
    int inner = w - 2, len = inner * value / max;
    if (!bipolar) {
        gfx_fill(x + 1, y + 1, len, h - 2, 1);
        return;
    }
    int mid = inner / 2;
    if (len >= mid)
        gfx_fill(x + 1 + mid, y + 1, len - mid + 1, h - 2, 1);
    else
        gfx_fill(x + 1 + len, y + 1, mid - len + 1, h - 2, 1);
}
