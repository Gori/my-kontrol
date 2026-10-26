/* XS1 (XMOS xCORE, Komplete Kontrol MK1) tile 0: entry from crt0.s. Tile 0 runs the core
   (core/main.c), the board and its drivers; tile 1 (tile1.c) runs USB, the 5-pin UART and
   the flash, behind the tile link (link.h). */
#include "hal.h"

void mk_main(void);
void tile0_link_open(void);
void xs1_boot_combo(void);

int main(void)
{
    tile0_link_open();          /* first chanend of the tile: the link (link.h) */
    board_early_init();
    if (board_boot_combo())
        xs1_boot_combo();           /* update mode, or held 10 s: the factory image */
    mk_main();
    return 0;
}
