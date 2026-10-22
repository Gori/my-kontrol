#pragma once
/* Monochrome framebuffer (DISP_W x DISP_H) in 8-pixel row bands, LSB = top. */
#include <stdint.h>
#include "font.h"

extern const font_t font_small;  /* 5x8 */
extern const font_t font_large;  /* 8x16 */
extern const font_t font_huge;   /* 12x24 */

extern uint8_t gfx_fb[];

void gfx_clear(void);
void gfx_pixel(int x, int y, int on);
void gfx_fill(int x, int y, int w, int h, int on);
void gfx_frame(int x, int y, int w, int h);
void gfx_hline_dotted(int x, int y, int w);
void gfx_invert(int x, int y, int w, int h);
int gfx_text(int x, int y, const char *s, const font_t *f);  /* returns end x */
int gfx_text_n(int x, int y, const char *s, int max_chars, const font_t *f);
int gfx_text_width(const char *s, const font_t *f);
void gfx_text_center(int y, const char *s, const font_t *f);
void gfx_text_right(int right, int y, const char *s, const font_t *f);
void gfx_bar(int x, int y, int w, int h, int value, int max, int bipolar);
