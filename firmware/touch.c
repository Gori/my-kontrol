#include "touch.h"
#include "hw.h"
#include "i2c_bb.h"

static const i2c_bus_t bus = {0x40010C00, 10, 11, 10};   /* PB10 SCL, PB11 SDA */

/* Two chip variants seen in the stock firmware: 0x15 with an init sequence
   and mask register 0x22, or 0x50 with no init and mask register 0xA0. */
static uint8_t addr, reg, ok;
static uint8_t last_raw, stable;

int touch_init(void)
{
    static const uint8_t init[][2] = {{0xAF, 0x3A}, {0x1F, 0x00}, {0x12, 0x18}, {0x1E, 0xE0}, {0x1F, 0x01}};
    RCC_APB2ENR |= 1 << 3;
    i2c_init(&bus);
    for (int attempt = 0; attempt < 10 && !ok; attempt++) {
        int good = 1;
        for (unsigned i = 0; i < sizeof init / sizeof init[0]; i++)
            good &= i2c_write_reg(&bus, 0x15, init[i][0], &init[i][1], 1) == 0;
        if (good) {
            addr = 0x15;
            reg = 0x22;
            ok = 1;
            break;
        }
        uint8_t v;
        if (i2c_read_reg(&bus, 0x50, 0xA0, &v, 1) == 0) {
            addr = 0x50;
            reg = 0xA0;
            ok = 1;
            break;
        }
        delay_ms(5);
    }
    return ok ? 0 : -1;
}

int touch_poll(uint8_t *mask)
{
    uint8_t v;
    if (!ok || i2c_read_reg(&bus, addr, reg, &v, 1))
        return 0;
    if (v != last_raw) {              /* debounce: two equal reads */
        last_raw = v;
        return 0;
    }
    if (v == stable)
        return 0;
    stable = v;
    *mask = v;
    return 1;
}
