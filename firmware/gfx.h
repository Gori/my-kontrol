#pragma once
#include <stdint.h>
#include "font.h"

extern const font_t font_small;  /* 5x8 */
extern const font_t font_large;  /* 8x16 */

void gfx_clear(void);
void gfx_pixel(int x, int y, int on);
void gfx_fill(int x, int y, int w, int h, int on);
void gfx_invert(int x, int y, int w, int h);
int gfx_text(int x, int y, const char *s, const font_t *f);  /* returns end x */
int gfx_text_width(const char *s, const font_t *f);
void gfx_text_center(int y, const char *s, const font_t *f);
void gfx_flush(void);  /* sends the framebuffer to the display */
