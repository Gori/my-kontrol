#include "pedal.h"
#include "hw.h"
#include "hal.h"
#include "knobs.h"

#define GPIOB_IDR  REG(0x40010C08)
#define GPIOC_CRL  REG(0x40011000)
#define GPIOC_BSRR REG(0x40011010)

#define SETTLE_POLLS 50          /* 500 ms after plug-in: learn the released level */
#define PRESS   0x500            /* hysteresis around the released level */
#define RELEASE 0x300

uint16_t pedal_raw[3];
static uint16_t rest[2];
static uint8_t state[2], settle, plugged;

/* pin = 4 or 5: make it analog, drive the other one high */
static uint16_t sample(int pin)
{
    int other = pin == 4 ? 5 : 4;
    uint32_t crl = GPIOC_CRL & ~(0xFFu << 16);
    crl |= 0x0u << (pin * 4);                 /* analog */
    crl |= 0x2u << (other * 4);               /* push-pull out */
    GPIOC_BSRR = 1u << other;
    GPIOC_CRL = crl;
    for (volatile int i = 0; i < 200; i++)
        ;
    return knobs_adc(pin == 4 ? 14 : 15);
}

void pedal_init(void)
{
    RCC_APB2ENR |= (1 << 3) | (1 << 4);
    /* PB12 is configured as floating input by input_init */
    settle = SETTLE_POLLS;
}

int pedal_poll(uint8_t *sw, uint8_t *down)
{
    int p = (GPIOB_IDR >> 12) & 1;             /* inferred: high = plug inserted */
    pedal_raw[0] = p;
    pedal_raw[1] = sample(4);
    pedal_raw[2] = sample(5);
    if (p != plugged) {
        plugged = p;
        settle = SETTLE_POLLS;
        state[0] = state[1] = 0;
    }
    if (!plugged)
        return 0;
    if (settle) {
        settle--;
        rest[0] = pedal_raw[1];
        rest[1] = pedal_raw[2];
        return 0;
    }
    for (int s = 0; s < 2; s++) {
        int d = pedal_raw[1 + s] - rest[s];
        if (d < 0) d = -d;
        if (!state[s] && d > PRESS) state[s] = 1;
        else if (state[s] && d < RELEASE) state[s] = 0;
        else continue;
        *sw = s;
        *down = state[s];
        return 1;
    }
    return 0;
}
