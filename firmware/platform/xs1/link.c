/* Tile link packets, both tiles (link.h). */
#include <string.h>
#include "link.h"
#include "xs1.h"

void link_send(uint32_t c, uint8_t type, const uint8_t *p, uint32_t n)
{
    xs1_outt(c, type);
    xs1_outt(c, n);
    for (uint32_t i = 0; i < n; i++)
        xs1_outt(c, p[i]);
    xs1_outct(c, CT_END);
}

uint8_t link_recv(uint32_t c, uint8_t *p, uint32_t *n)
{
    uint8_t type = (uint8_t)xs1_int(c);
    *n = xs1_int(c);
    for (uint32_t i = 0; i < *n; i++)
        p[i] = (uint8_t)xs1_int(c);
    xs1_chkct(c, CT_END);
    return type;
}

void link_chip_id(const uint8_t ident[64], uint8_t out[12])
{
    for (int k = 0; k < 3; k++) {   /* FNV-1a, 3 seeds */
        uint32_t h = 2166136261u + (uint32_t)k;
        for (int i = 0; i < 64; i++)
            h = (h ^ ident[i]) * 16777619u;
        memcpy(out + 4 * k, &h, 4);
    }
}
