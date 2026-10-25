/* Keybed scanner messages and velocity (ni_keybed.h). Our code. */
#include "ni_keybed.h"

int ni_keybed_byte(ni_keybed_t *k, uint8_t b, uint8_t *note, uint8_t *on, uint16_t *raw)
{
    if (b & 0x80) {                         /* a status byte starts a message */
        k->b[0] = b;
        k->n = 1;
        return 0;
    }
    if (!k->n)
        return 0;                           /* data without a status: wait for the next */
    k->b[k->n++] = b;
    if (k->n < 3)
        return 0;
    k->n = 0;
    uint8_t st = k->b[0] & 0xF8;
    if (st != 0x90 && st != 0x80)
        return 0;
    *note = k->b[1];
    *on = st == 0x90;
    *raw = (uint16_t)((k->b[0] & 7) << 7 | k->b[2]);
    return 1;
}

/* Our curve, from the raw value (a key travel time, smaller = faster): velocity falls with
   the logarithm of the time, 127 at 28 and below, 1 at 650 and above. Points in between
   are interpolated linearly. */
static const uint16_t raw_at[] = {28, 40, 60, 100, 150, 200, 300, 400, 500, 650};
static const uint8_t vel_at[] = {127, 118, 101, 80, 63, 50, 33, 21, 12, 1};

uint8_t ni_keybed_velocity(uint16_t raw)
{
    if (raw <= raw_at[0])
        return vel_at[0];
    for (unsigned i = 1; i < sizeof raw_at / sizeof raw_at[0]; i++)
        if (raw <= raw_at[i]) {
            int span = raw_at[i] - raw_at[i - 1];
            int v = vel_at[i - 1] - (vel_at[i - 1] - vel_at[i]) * (raw - raw_at[i - 1]) / span;
            return (uint8_t)(v < 1 ? 1 : v);
        }
    return 1;
}
