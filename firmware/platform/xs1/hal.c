/* XS1 tile 0: the platform side of core/hal.h. Tile 0 runs the core, the board and its
   drivers; USB, the 5-pin UART and the SPI flash are on tile 1, reached over the tile link
   (link.h, tile0_link.c).

   - Time: the 100 MHz reference timer, extended to milliseconds since boot (the 32-bit
     timer wraps every 42.9 s; hal_ms() must be called more often than that, the main loop
     does).
   - Tick: hal_tick_start() runs board_tick() every 100 us on its own hardware thread. Unlike
     the STM32's interrupt it runs *alongside* the main loop, not instead of it.
   - Flash: the settings pages are a RAM mirror (xs1_flash_mirror, board_config.h) loaded
     from the SPI flash at start; erase and write go to both (tile0_link.c).
   - Not done yet: watchdog (none used), the crash counter and the way back to the
     factory image, reboot and the update mode (they park the core), a unique ID (fixed). */
#include "hal.h"
#include "xs1.h"

static uint32_t tmr, last_tick, ms_acc;

static void time_init(void)
{
    if (!tmr) {
        tmr = xs1_getr(RES_TIMER);
        last_tick = xs1_time(tmr);
    }
}

uint32_t hal_ms(void)
{
    time_init();
    uint32_t now = xs1_time(tmr), n = (now - last_tick) / TICKS_PER_MS;
    ms_acc += n;
    last_tick += n * TICKS_PER_MS;
    return ms_acc;
}

void hal_delay_ms(uint32_t ms)
{
    time_init();
    xs1_wait_until(tmr, xs1_time(tmr) + ms * TICKS_PER_MS);
}

void hal_watchdog_feed(void)
{
}

void hal_mark_stable(void)
{
}

void hal_unique_id(uint8_t out[12])
{
    for (int i = 0; i < 12; i++)
        out[i] = 0x49;              /* XS1 has no chip ID; USB uses board_serial() */
}

void hal_usb_init(void)
{
}

static uint32_t tick_stack[64];

static void tick_thread(uint32_t arg)
{
    (void)arg;
    uint32_t t = xs1_getr(RES_TIMER), next = xs1_time(t);
    for (;;) {
        next += 100 * TICKS_PER_US;
        xs1_wait_until(t, next);
        board_tick();
    }
}

void hal_tick_start(void)
{
    xs1_thread_start(tick_thread, 0, tick_stack, sizeof tick_stack / 4);
}

static void park(void) __attribute__((noreturn));
static void park(void)
{
    for (;;)
        asm volatile("clre\n\twaiteu");
}

void hal_enter_bootloader(void)
{
    park();                         /* TODO: XMOS DFU via tile 1 */
}

void hal_reboot(void)
{
    park();                         /* TODO: chip reset through the switch */
}
