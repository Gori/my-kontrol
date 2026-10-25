/* LED chain planes (shift595.h). Our code. */
#include "shift595.h"

void shift595_plane(const uint8_t *level, int nbits, int k, uint8_t *out)
{
    int nbytes = nbits / 8;
    for (int j = 0; j < nbytes; j++) {
        const uint8_t *l = level + 8 * (nbytes - 1 - j);    /* last byte first */
        uint8_t b = 0xFF;                                     /* active low: 1 = off */
        for (int i = 0; i < 8; i++)
            if (l[i] >> k & 1)
                b &= (uint8_t)~(1u << i);
        out[j] = b;
    }
}
