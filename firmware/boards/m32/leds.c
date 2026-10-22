#include "leds.h"
#include "hw.h"
#include "hal.h"
#include "i2c_bb.h"

#define GPIOA_CRH  REG(0x40010804)
#define GPIOA_BSRR REG(0x40010810)
#define ADDR 0x3C

static const i2c_bus_t bus = {0x40010C00, 6, 7, 10};   /* PB6 SCL, PB7 SDA */

/* LED index -> driver channel (stock table at 0x0800BD4A) */
static const uint8_t channel[LED_COUNT] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0B, 0x0C,
    0x0D, 0x09, 0x0A, 0x0E, 0x0F, 0x12, 0x11, 0x10, 0x13, 0x14,
};

static uint8_t pwm[LED_COUNT];      /* by channel */
static uint8_t dirty, ok;

int leds_init(void)
{
    RCC_APB2ENR |= (1 << 2) | (1 << 3);
    GPIOA_CRH = (GPIOA_CRH & ~0xF0000000u) | 0x20000000;   /* PA15 output */
    GPIOA_BSRR = 1 << 15;                                   /* driver enable */
    hal_delay_ms(1);
    i2c_init(&bus);

    uint8_t v = 0;
    ok = i2c_write_reg(&bus, ADDR, 0x4F, &v, 1) == 0;       /* reset */
    v = 1;
    ok &= i2c_write_reg(&bus, ADDR, 0x00, &v, 1) == 0;      /* normal operation */
    uint8_t ctl[36];
    for (int i = 0; i < 36; i++)
        ctl[i] = 0x07;                                      /* channel on, I_MAX/4 (stock) */
    ok &= i2c_write_reg(&bus, ADDR, 0x26, ctl, 36) == 0;
    dirty = 1;
    leds_flush();
    return ok ? 0 : -1;
}

void leds_set(uint8_t idx, uint8_t v)
{
    if (idx >= LED_COUNT)
        return;
    uint8_t ch = channel[idx];
    if (pwm[ch] != v) {
        pwm[ch] = v;
        dirty = 1;
    }
}

int leds_ok(void)
{
    return ok;
}

void leds_power(int on)
{
    GPIOA_BSRR = on ? 1u << 15 : 1u << 31;
}

void leds_all(uint8_t v)
{
    for (int i = 0; i < LED_COUNT; i++)
        leds_set(i, v);
}

void leds_flush(void)
{
    if (!dirty || !ok)
        return;
    dirty = 0;
    uint8_t one = 1;
    i2c_write_reg(&bus, ADDR, 0x01, pwm, LED_COUNT);
    i2c_write_reg(&bus, ADDR, 0x25, &one, 1);               /* latch */
}
