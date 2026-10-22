#pragma once
/* 128x32 SSD1306-class OLED on SPI1 (docs/hardware/m32.md section 7). */
#include <stdint.h>

void display_init(void);
void display_power(int on);
void display_contrast(uint8_t v);
void display_page(int page, const uint8_t *cols);   /* 128 bytes, LSB = top pixel */
