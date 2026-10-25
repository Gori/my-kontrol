/* XS1 tile 0: the platform side of core/hal.h. Tile 0 runs the core, the board and its
   drivers; USB, the 5-pin UART and the SPI flash are on tile 1, reached over the tile link
   (link.h, tile0_link.c).

   - Time: the 100 MHz reference timer, extended to milliseconds since boot (the 32-bit
     timer wraps every 42.9 s; hal_ms() must be called more often than that, the main loop
     does).
   - Tick: hal_tick_start() runs board_tick() every 100 us on its own hardware thread. Unlike
     the STM32's interrupt it runs *alongside* the main loop, not instead of it.
   - Watchdog: the XS1 has none. The tick thread checks every 250 ms that the main loop fed
     it, the main loop checks that the tick runs; 2 s without either reboots the chip.
   - Crash counter: a boot log in its own SPI flash sector (BOARD_SPI_BOOTLOG). Every start
     appends BOOT, hal_mark_stable() (10 s up) appends STABLE. 3 BOOTs in a row before this
     one = 3 starts that never got stable: the log is cleared and the core stops in safe
     mode instead of starting a 4th time. Power cycles count too (the XS1 tells no reset
     cause), so 3 quick ones are a way into safe mode; the next start runs normally.
   - Reboot: a write to the switch's PLL register (sswitch.c), as NI's firmware does.
   - Update mode and safe mode: for now both stop the core with a marker on the displays
     (TODO: XMOS DFU through tile 1). Tile 1 keeps running.
   - Unique ID: the XS1 has no chip ID; a hash of NI's identity sector (flash 0x4C000,
     read-only for us), which differs per keyboard if it holds the serial (TODO: confirm).
   - Flash: the settings pages are a RAM mirror (xs1_flash_mirror, board_config.h) loaded
     from the SPI flash at start; erase and write go to both (tile0_link.c). */
#include <string.h>
#include "hal.h"
#include "link.h"
#include "xs1.h"

#ifndef BOARD_SPI_BOOTLOG
#define BOARD_SPI_BOOTLOG (BOARD_SPI_SETTINGS + 0x4000)   /* after the 4 settings sectors */
#endif
#define SPI_IDENTITY 0x4C000u
#define LOG_SIZE 4096u
#define LOG_BOOT 0xB0
#define LOG_STABLE 0x5A
#define CRASH_LIMIT 3
#define WD_PERIOD_MS 250
#define WD_MISSES 8

static uint32_t tmr, last_tick, ms_acc;
static volatile uint32_t wd_feeds, wd_ticks;
static volatile uint8_t wd_off;
static uint32_t log_end;
static uint8_t chip_id[12];

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
    uint32_t t = xs1_time(tmr);
    while (ms--) {
        t += TICKS_PER_MS;
        xs1_wait_until(tmr, t);
        hal_watchdog_feed();
    }
}

/* the main loop feeds; it also reboots when the tick thread has stopped for 2 s */
void hal_watchdog_feed(void)
{
    static uint32_t ticks, since;
    wd_feeds++;
    uint32_t now = hal_ms();
    if (wd_ticks != ticks || wd_off)
        ticks = wd_ticks, since = now;
    else if (now - since > WD_MISSES * WD_PERIOD_MS)
        hal_reboot();
}

void hal_unique_id(uint8_t out[12])
{
    memcpy(out, chip_id, 12);
}

void hal_usb_init(void)
{
}

/* ---------- the tick thread, which also watches the main loop ---------- */

static uint32_t tick_stack[64];

static void tick_thread(uint32_t arg)
{
    (void)arg;
    uint32_t t = xs1_getr(RES_TIMER), next = xs1_time(t), feeds = wd_feeds;
    int n = 0, misses = 0;
    for (;;) {
        next += 100 * TICKS_PER_US;
        xs1_wait_until(t, next);
        board_tick();
        wd_ticks++;
        if (++n < WD_PERIOD_MS * 10)
            continue;
        n = 0;
        misses = wd_feeds == feeds && !wd_off ? misses + 1 : 0;
        feeds = wd_feeds;
        if (misses >= WD_MISSES)
            hal_reboot();
    }
}

void hal_tick_start(void)
{
    xs1_thread_start(tick_thread, 0, tick_stack, sizeof tick_stack / 4);
}

/* ---------- stopping and rebooting ---------- */

static void stop(const char *a, const char *b) __attribute__((noreturn));
static void stop(const char *a, const char *b)
{
    wd_off = 1;
#if BOARD_SEG_DISPLAYS
    seg_display_t d[BOARD_SEG_DISPLAYS];
    memset(d, 0, sizeof d);
    for (int i = 0; i < BOARD_SEG_DISPLAYS; i++)
        memset(d[i].line, ' ', sizeof d[i].line);
    memcpy(d[0].line[0], a, strlen(a));
    memcpy(d[0].line[1], b, strlen(b));
    board_seg_show(d);
#else
    (void)a, (void)b;               /* the core has drawn its update screen */
#endif
    for (;;)
        asm volatile("clre\n\twaiteu");
}

void hal_enter_bootloader(void)
{
    stop("UPDATE", "MODE");         /* TODO: XMOS DFU via tile 1 */
}

void hal_reboot(void)
{
    wd_off = 1;
    xs1_chip_reset();
}

/* ---------- at link-up (tile0_link.c usb_init): the boot log and the ID ---------- */

static void log_append(uint8_t b)
{
    if (log_end >= LOG_SIZE) {      /* full: start the sector over */
        tile0_flash_erase(BOARD_SPI_BOOTLOG);
        log_end = 0;
    }
    if (tile0_flash_write(BOARD_SPI_BOOTLOG + log_end, &b, 1) == 0)
        log_end++;
}

void hal_mark_stable(void)
{
    log_append(LOG_STABLE);
}

void xs1_link_up(void)
{
    uint8_t buf[64];
    tile0_flash_read(SPI_IDENTITY, buf, sizeof buf);
    for (int k = 0; k < 3; k++) {   /* FNV-1a, 3 seeds */
        uint32_t h = 2166136261u + (uint32_t)k;
        for (uint32_t i = 0; i < sizeof buf; i++)
            h = (h ^ buf[i]) * 16777619u;
        memcpy(chip_id + 4 * k, &h, 4);
    }

    uint32_t crashes = 0;
    for (log_end = 0; log_end < LOG_SIZE; log_end += sizeof buf) {
        tile0_flash_read(BOARD_SPI_BOOTLOG + log_end, buf, sizeof buf);
        uint32_t i = 0;
        for (; i < sizeof buf && buf[i] != 0xFF; i++)
            crashes = buf[i] == LOG_BOOT ? crashes + 1 : 0;
        if (i < sizeof buf) {
            log_end += i;
            break;
        }
    }
    if (crashes >= CRASH_LIMIT) {
        tile0_flash_erase(BOARD_SPI_BOOTLOG);
        stop("SAFE", "MODE");
    }
    log_append(LOG_BOOT);
}
