#include "strips.h"
#include "hw.h"
#include "i2c_bb.h"

#define GPIOA_CRH  REG(0x40010804)
#define GPIOA_BSRR REG(0x40010810)
#define ADDR 0x15

static const i2c_bus_t bus[2] = {
    {0x40011000, 1, 0, 10},    /* pitch: PC1 SCL, PC0 SDA */
    {0x40011000, 3, 2, 10},    /* mod:   PC3 SCL, PC2 SDA */
};
static const uint8_t reset_pin[2] = {10, 9};   /* PA10 pitch, PA9 mod; active low */

uint8_t strips_probe_raw[2][3];

int strips_init(void)
{
    int ok = 0;
    RCC_APB2ENR |= (1 << 2) | (1 << 4);
    /* PA9/PA10 open-drain outputs, released (chips running) */
    GPIOA_BSRR = (1 << 9) | (1 << 10);
    GPIOA_CRH = (GPIOA_CRH & ~0x00000FF0u) | 0x00000770;
    for (int s = 0; s < 2; s++)
        i2c_init(&bus[s]);
    delay_ms(20);
    for (int s = 0; s < 2; s++) {
        uint8_t r[3] = {0xEE, 0xEE, 0xEE};
        if (i2c_read_reg(&bus[s], ADDR, 0x00, r, 3) == 0)
            ok |= 1 << s;
        for (int i = 0; i < 3; i++)
            strips_probe_raw[s][i] = r[i];
    }
    (void)reset_pin;
    return ok;
}

int strips_read(int s, uint8_t *pos)
{
    uint8_t r[3];
    if (i2c_read_reg(&bus[s], ADDR, 0x00, r, 3))
        return -1;
    /* stock decoding: n, p1, p2; two fingers -> second position */
    unsigned p = (r[1] == 0 && r[2] == 0) ? 0 : (r[0] > 1 ? r[2] : r[1]);
    if (p < 58)
        *pos = p ? 1 : 0;
    else {
        unsigned v = (p - 57) * 6 / 5;
        *pos = v > 200 ? 200 : v;
    }
    return 0;
}
