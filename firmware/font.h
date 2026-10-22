#pragma once
#include <stdint.h>

typedef struct {
    uint8_t w, h;          /* cell size; w <= 8 */
    uint8_t first, count;  /* ASCII range */
    const uint8_t *data;   /* h bytes per glyph, MSB = leftmost pixel */
} font_t;
