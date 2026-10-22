#pragma once
#include <stdint.h>

typedef struct {
    uint8_t w, h;          /* cell size */
    uint8_t first, count;  /* ASCII range */
    const uint8_t *data;   /* h rows per glyph, (w + 7) / 8 bytes per row, MSB = leftmost pixel */
} font_t;
