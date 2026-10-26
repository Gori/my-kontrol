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
   - Update mode: tile 1 reboots the chip into its DFU device (tile1.c); this tile shows
     UPDATE MODE and waits. Both page buttons at power-up do the same; held 10 s more, they
     remove the upgrade image so NI's factory image boots (xs1_boot_combo).
   - Safe mode: the core stops with a marker on the displays; tile 1 (USB, DFU) runs on.
   - Unique ID: the XS1 has no chip ID; a hash of NI's identity sector (flash 0x4C000,
     read-only for us), which differs per keyboard if it holds the serial (TODO: confirm).
   - Flash: the settings store reads, erases and writes tile 1's SPI flash by request
     (BOARD_FLASH_READ, tile0_link.c). */
#include <string.h>
#include "hal.h"
#include "link.h"
#include "xs1.h"

#ifndef BOARD_SPI_BOOTLOG
#define BOARD_SPI_BOOTLOG (BOARD_SPI_SETTINGS + 0x4000)   /* after the 4 settings sectors */
#endif
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

static uint32_t tick_stack[164];       /* 92 bytes used at most, reboot included; xtest wants 512 free */

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

/* ---------- stopping, update mode, rebooting ---------- */

/* display 1 shows a / b, display 2 c / d (segment boards; others keep the core's screen) */
static void show(const char *a, const char *b, const char *c, const char *d)
{
#if BOARD_SEG_DISPLAYS
    seg_display_t s[BOARD_SEG_DISPLAYS];
    const char *t[4] = {a, b, c, d};
    memset(s, 0, sizeof s);
    for (int i = 0; i < BOARD_SEG_DISPLAYS; i++)
        memset(s[i].line, ' ', sizeof s[i].line);
    for (int i = 0; i < 4; i++)
        if (t[i])
            memcpy(s[i / 2].line[i % 2], t[i], strlen(t[i]));
    board_seg_show(s);
#else
    (void)a, (void)b, (void)c, (void)d;
#endif
}

static void park(void) __attribute__((noreturn));
static void park(void)
{
    for (;;)
        asm volatile("clre\n\twaiteu");
}

static void stop(const char *a, const char *b) __attribute__((noreturn));
static void stop(const char *a, const char *b)
{
    wd_off = 1;
    show(a, b, 0, 0);
    park();
}

/* tile 1 came up in update mode (its DFU device): nothing to do here */
void xs1_update_mode(void) __attribute__((noreturn));
void xs1_update_mode(void)
{
    stop("UPDATE", "MODE");
}

/* Tile 1 reboots the chip into update mode. If it doesn't within 1 s (tile 1 not up, or
   not answering), the core stops here with the marker. */
void hal_enter_bootloader(void)
{
    wd_off = 1;
    show("UPDATE", "MODE", 0, 0);
    if (tile0_link_is_up())
        tile0_system(LINK_SYS_UPDATE);
    hal_delay_ms(1000);
    park();
}

/* Both page buttons held at power-up (platform.c): update mode, or, still held after 10 s,
   back to the factory image (NI's firmware) without USB: tile 1 removes the upgrade image
   and reboots. For a keyboard whose USB doesn't come up. */
void xs1_boot_combo(void)
{
    char n[9] = "IN    S";
    wd_off = 1;
    tile0_link_wait();
    for (int s = 10; s > 0; s--) {
        n[3] = (char)('0' + s / 10), n[4] = (char)('0' + s % 10);
        if (n[3] == '0')
            n[3] = ' ';
        show("UPDATE", "MODE", "FACTORY", n);
        for (int i = 0; i < 10; i++) {
            hal_delay_ms(100);
            if (!board_boot_combo())
                hal_enter_bootloader();
        }
    }
    show("FACTORY", "RESTORE", 0, 0);
    if (tile0_system(LINK_SYS_FACTORY))
        show("FACTORY", "FAILED", 0, 0);
    park();
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
    tile0_flash_read(LINK_ID_FLASH, buf, sizeof buf);
    link_chip_id(buf, chip_id);

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
