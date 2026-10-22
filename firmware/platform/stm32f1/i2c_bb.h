#pragma once
/* Bit-banged I2C master on open-drain GPIOs, with clock-stretch timeouts
   (the stock firmware waits forever; a stuck chip would trip the watchdog). */
#include <stdint.h>

typedef struct {
    uint32_t port;      /* GPIO base, e.g. 0x40010C00 */
    uint8_t scl, sda;   /* pin numbers */
    uint8_t delay;      /* half-period busy loops */
} i2c_bus_t;

void i2c_init(const i2c_bus_t *b);
/* 7-bit address. Return 0 on success, -1 on NACK or timeout. */
int i2c_write(const i2c_bus_t *b, uint8_t addr, const uint8_t *data, int n);
int i2c_write_reg(const i2c_bus_t *b, uint8_t addr, uint8_t reg, const uint8_t *data, int n);
int i2c_read_reg(const i2c_bus_t *b, uint8_t addr, uint8_t reg, uint8_t *data, int n);
