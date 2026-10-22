#pragma once
/* 21 button LEDs on an IS31FL3236-type driver (I2C 0x3C on PB6/PB7, PA15 = enable).
   LED index = button index for the backlit buttons (docs/hardware/m32.md 5). */
#include <stdint.h>

#define LED_COUNT 21
int leds_init(void);                 /* 0 if the driver answered */
int leds_ok(void);
void leds_power(int on);             /* driver enable (off during USB suspend) */
void leds_set(uint8_t idx, uint8_t pwm);
void leds_all(uint8_t pwm);
void leds_flush(void);               /* sends changes; call from the main loop */
