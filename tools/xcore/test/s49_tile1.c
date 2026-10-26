/* Tile 1 test program for the S49 harness: the tile-1 side of the tile link (contract:
   HostLink in tools/xcore/s49emu.py) with no USB: sends USB_STATE 0 after 1 ms and sends
   DIN_TX bytes out of the 5-pin MIDI OUT (port 1J, inverted like NI's firmware, 31 250 Bd 8N1,
   timed port outputs). */
#include <stdint.h>
#include "xs1io.h"

#define PORT_1J 0x10900u
#define BIT 3200u                   /* reference ticks per bit */

static void din_byte(uint8_t b)
{
    uint32_t frame = (uint32_t)b << 1 | 0x200, t;
    out(PORT_1J, 1);
    t = getts(PORT_1J);
    for (int i = 0; i < 10; i++) {
        t += BIT;
        setpt(PORT_1J, t);
        out(PORT_1J, frame >> i & 1);
    }
}

int main(void)
{
    uint8_t p[256];
    uint32_t n, link = xs1_chanend0;            /* chanend 0 (crt0.s) = LINK_TILE1 */
    uint32_t tmr = getr(RES_TIMER);
    setd(link, LINK_TILE0);
    setc(PORT_1J, SETC_INUSE_ON);
    setc(PORT_1J, SETC_INV);
    out(PORT_1J, 1);
    wait_until(tmr, in(tmr) + 100000);          /* 1 ms: tile 0 has allocated its end */
    uint8_t st = 0;
    link_send(link, LINK_USB_STATE, &st, 1);
    for (;;) {
        uint32_t type = link_recv(link, p, &n);
        if (type == LINK_DIN_TX) {
            for (uint32_t i = 0; i < n; i++)
                din_byte(p[i]);
        }
    }
}
