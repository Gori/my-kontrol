/* Host implementations of core/hal.h: RAM flash with NOR rules, a manual
   clock, recorded MIDI and lights, and a framebuffer sink. */
#include <stdio.h>
#include <string.h>
#include "fake_hal.h"
#include "midi.h"

uint8_t fake_flash[5 * 1024];
uint32_t fake_now_ms;
int fake_flash_fail_after = -1;     /* simulate a power cut after N more half-words */
uint8_t fake_lights[64];
int fake_display_on = 1;

#ifndef FAKE_CAPS
#define FAKE_CAPS 0x1FF                     /* everything; 0xF = the M32's */
#endif
const board_info_t board_info = {"Host Board", "HOST", 99, BOARD_KEYS, 41, BOARD_KNOBS, FAKE_CAPS};

void fake_reset(void)
{
    memset(fake_flash, 0xFF, sizeof fake_flash);
    fake_now_ms = 0;
    fake_flash_fail_after = -1;
}

uint32_t hal_ms(void) { return fake_now_ms; }
void hal_delay_ms(uint32_t ms) { fake_now_ms += ms; }
void hal_watchdog_feed(void) {}
void hal_unique_id(uint8_t out[12]) { memset(out, 0x42, 12); }
void hal_tick_start(void) {}
void hal_mark_stable(void) {}
void hal_usb_init(void) {}

static int in_flash(uintptr_t a, uint32_t n)
{
    return a >= (uintptr_t)fake_flash && a + n <= (uintptr_t)fake_flash + sizeof fake_flash;
}

void hal_flash_read(uintptr_t addr, void *dst, uint32_t n)
{
    memcpy(dst, (const void *)addr, n);
}

int hal_flash_erase(uintptr_t addr)
{
    if (!in_flash(addr, FLASH_PAGE) || (addr - (uintptr_t)fake_flash) % FLASH_PAGE)
        return -1;
    memset((void *)addr, 0xFF, FLASH_PAGE);
    return 0;
}

int hal_flash_write(uintptr_t addr, const void *src, uint32_t len)
{
    if (!in_flash(addr, len))
        return -1;
    const uint8_t *s = src;
    uint8_t *d = (uint8_t *)addr;
    for (uint32_t i = 0; i < len; i += 2) {
        if (fake_flash_fail_after == 0)
            return -1;                       /* power is gone */
        if (fake_flash_fail_after > 0)
            fake_flash_fail_after--;
        for (uint32_t k = i; k < i + 2 && k < len; k++) {
            if (d[k] != 0xFF && d[k] != s[k]) {
                fprintf(stderr, "flash: overwrite without erase at %+ld\n", (long)(addr + k - (uintptr_t)fake_flash));
                return -1;
            }
            d[k] = s[k];
        }
    }
    return 0;
}

void hal_enter_bootloader(void) { printf("[enter bootloader]\n"); for (;;) ; }
void hal_reboot(void) { for (;;) ; }

void board_early_init(void) {}
int board_boot_combo(void) { return 0; }
void board_init(void) {}
void board_tick(void) {}
int board_poll(mk_event_t *ev) { (void)ev; return 0; }
void board_light(uint8_t ctl, uint8_t level) { if (ctl < 64) fake_lights[ctl] = level; }
void board_lights_flush(void) {}
void board_display_page(int page, const uint8_t *cols) { (void)page; (void)cols; }
void board_display_contrast(uint8_t v) { (void)v; }
void board_display_power(int on) { fake_display_on = on; }
void board_usb_pullup(int on) { (void)on; }
int board_diag(uint8_t *buf, int max) { (void)buf; (void)max; return 0; }
int board_serial(char *out, int max) { (void)out; (void)max; return 0; }

/* drain the MIDI out queues into a byte log per port */
uint8_t fake_midi[PORT_COUNT][4096];
int fake_midi_n[PORT_COUNT];

void fake_drain_midi(void)
{
    static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    uint8_t p[4];
    while (midi_tx_peek(p)) {
        int port = p[0] >> 4, n = len[p[0] & 15];
        for (int i = 0; i < n && fake_midi_n[port] < (int)sizeof fake_midi[0]; i++)
            fake_midi[port][fake_midi_n[port]++] = p[1 + i];
        midi_tx_pop();
    }
#if BOARD_MIDI_DIN
    for (int b; (b = midi_din_tx_byte()) >= 0; )
        if (fake_midi_n[PORT_DIN] < (int)sizeof fake_midi[0])
            fake_midi[PORT_DIN][fake_midi_n[PORT_DIN]++] = b;
#endif
}

void fake_clear_midi(void)
{
    fake_drain_midi();
    for (int i = 0; i < PORT_COUNT; i++)
        fake_midi_n[i] = 0;
}

void kbd_tap(uint8_t modifier, uint8_t key) { (void)modifier; (void)key; }

/* a short surface description; DESCRIBE splits it into several parts */
const char board_surface_json[] =
    "{\"schema\":1,\"board\":99,\"name\":\"Host Board\",\"knobs\":{\"count\":8,\"steps_per_turn\":100},"
    "\"controls\":[{\"id\":0,\"name\":\"shift\",\"kind\":\"button\",\"light\":true},"
    "{\"id\":9,\"name\":\"play\",\"kind\":\"button\",\"light\":true}]}";
