#pragma once
#include <stdint.h>

/* buf: 512 bytes, 4 pages of 128 columns, LSB = top pixel of the page */
void display_init(void);
void display_on(void);
void display_contrast(uint8_t v);
void display_blit(const uint8_t *buf);
