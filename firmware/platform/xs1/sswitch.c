/* XS1 switch registers through config packets, and the chip reset. A config packet goes to
   the node's switch (chanend ID node << 16 | 0xC30C): a WRITEC (0xC0) or READC (0xC1)
   control token, the return chanend (its ID >> 8, 3 bytes), the register (2 bytes), for a
   write the value (4 bytes), END; the switch answers ACK (3) [+ value] + END, or nothing
   when the return address is 0xFFFFFF. Writing the PLL control register (6) resets the
   chip; NI's firmware reboots that way, writing its own value back, the other node first. */
#include "xs1.h"

#define SSWITCH 0xC30Cu
#define CT_WRITEC 0xC0
#define CT_READC 0xC1
#define CT_ACK 3
#define REG_PLL_CTL 6

uint32_t xs1_node(void)
{
    uint32_t c = xs1_getr(RES_CHANEND), node = c >> 16;
    xs1_freer(c);
    return node;
}

int xs1_sswitch_read(uint32_t node, uint32_t reg, uint32_t *val)
{
    uint32_t c = xs1_getr(RES_CHANEND);
    xs1_setd(c, node << 16 | SSWITCH);
    xs1_outct(c, CT_READC);
    xs1_out(c, (c >> 8) << 8 | (reg >> 8 & 0xFF));
    xs1_outt(c, reg & 0xFF);
    xs1_outct(c, CT_END);
    int ok = xs1_inct(c) == CT_ACK;
    if (ok)
        *val = xs1_in(c);
    xs1_chkct(c, CT_END);
    xs1_freer(c);
    return ok ? 0 : -1;
}

void xs1_sswitch_write_noack(uint32_t node, uint32_t reg, uint32_t val)
{
    uint32_t c = xs1_getr(RES_CHANEND);
    xs1_setd(c, node << 16 | SSWITCH);
    xs1_outct(c, CT_WRITEC);
    xs1_out(c, 0xFFFFFF00u | (reg >> 8 & 0xFF));
    xs1_outt(c, reg & 0xFF);
    xs1_out(c, val);
    xs1_outct(c, CT_END);
    xs1_freer(c);
}

void xs1_chip_reset(void)
{
    uint32_t node = xs1_node(), pll = 0;
    xs1_sswitch_read(node, REG_PLL_CTL, &pll);
    xs1_sswitch_write_noack(node ^ 0x8000, REG_PLL_CTL, pll);
    xs1_sswitch_write_noack(node, REG_PLL_CTL, pll);
    for (;;)
        asm volatile("clre\n\twaiteu");
}
