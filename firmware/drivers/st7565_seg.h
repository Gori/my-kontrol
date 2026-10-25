#pragma once
/* ST7565-family controllers driving 16-segment glass, as on the Komplete Kontrol S MK1:
   3 controllers, 3 displays each, every display 2 lines of 8 characters with a dot after
   each character and 17 extra segments (the bar). Layout: docs/hardware/s-mk1.md,
   Displays (from fw/kks/notes/xmos.md §11).

   The driver has no pins: the board passes its bus operations. encode() is pure, so the
   segment mapping is host-tested (test/test_seg.c). */
#include <stdint.h>
#include "hal.h"            /* seg_display_t */

#define ST7565_SEG_CTRLS 3
#define ST7565_SEG_BYTES 240     /* display RAM used per controller: 2 pages x 120 columns */

typedef struct {
    void (*select)(uint8_t ctrls, int data);   /* chip-select controllers (bit n = n), A0 */
    void (*write)(uint8_t b);                  /* one byte to the selected ones, MSB first */
    void (*reset)(int active);                 /* the shared reset line */
    void (*delay_ms)(uint32_t ms);
} st7565_bus_t;

typedef uint8_t st7565_seg_ram_t[ST7565_SEG_CTRLS][ST7565_SEG_BYTES];

void st7565_seg_init(const st7565_bus_t *bus);             /* reset, configure, display on */
/* the RAM image of 9 displays: characters through font16, dots, bars of 17 segments */
void st7565_seg_encode(st7565_seg_ram_t ram, const seg_display_t d[9]);
void st7565_seg_send(const st7565_bus_t *bus, const st7565_seg_ram_t ram);
