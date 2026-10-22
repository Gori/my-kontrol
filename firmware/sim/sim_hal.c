/* core/hal.h for the simulator: the M32's identity, RAM flash, a clock
   driven by sim.c, and lights and display captured for the terminal UI. */
#include <string.h>
#include "sim.h"
#include "kbd.h"

uint8_t sim_flash[5 * 1024];
uint32_t sim_now_ms;
uint8_t sim_lights[CTL_COUNT];
uint8_t sim_display[DISP_H / 8][DISP_W];
int sim_display_on;
int sim_display_dirty;
char sim_kbd_last[32];
uint32_t sim_kbd_ms;

/* same as boards/m32/board.c, so IDENT and the About screen match */
const board_info_t board_info = {
    .name = "Komplete Kontrol M32",
    .short_name = "M32",
    .id = 1,
    .keys = BOARD_KEYS,
    .base_note = 41,
    .knobs = BOARD_KNOBS,
    .caps = CAP_STRIPS | CAP_KNOB_TOUCH | CAP_4D | CAP_PEDAL,
};

void sim_hal_init(void)
{
    memset(sim_flash, 0xFF, sizeof sim_flash);     /* erased: default settings */
    sim_now_ms = 0;
}

uint32_t hal_ms(void) { return sim_now_ms; }
void hal_delay_ms(uint32_t ms) { sim_now_ms += ms; }
void hal_watchdog_feed(void) {}
void hal_unique_id(uint8_t out[12]) { memcpy(out, "MKSIMULATOR1", 12); }
void hal_tick_start(void) {}
void hal_mark_stable(void) {}
void hal_usb_init(void) {}

static int in_flash(uintptr_t a, uint32_t n)
{
    return a >= (uintptr_t)sim_flash && a + n <= (uintptr_t)sim_flash + sizeof sim_flash;
}

int hal_flash_erase(uintptr_t addr)
{
    if (!in_flash(addr, FLASH_PAGE) || (addr - (uintptr_t)sim_flash) % FLASH_PAGE)
        return -1;
    memset((void *)addr, 0xFF, FLASH_PAGE);
    return 0;
}

int hal_flash_write(uintptr_t addr, const void *src, uint32_t len)
{
    if (!in_flash(addr, len))
        return -1;
    memcpy((void *)addr, src, len);
    return 0;
}

void hal_enter_bootloader(void)
{
    fprintf(stderr, "mk-sim: the keyboard asked for its bootloader; exiting\n");
    sim_quit(0);
}

void hal_reboot(void)
{
    fprintf(stderr, "mk-sim: the keyboard rebooted; exiting\n");
    sim_quit(0);
}

void board_early_init(void) {}
int board_boot_combo(void) { return 0; }
void board_init(void) {}
void board_tick(void) {}
int board_poll(mk_event_t *ev) { (void)ev; return 0; }   /* sim.c calls app_event() itself */
void board_light(uint8_t ctl, uint8_t level) { if (ctl < CTL_COUNT) sim_lights[ctl] = level; }
void board_lights_flush(void) {}

void board_display_page(int page, const uint8_t *cols)
{
    if (page >= 0 && page < DISP_H / 8 && memcmp(sim_display[page], cols, DISP_W)) {
        memcpy(sim_display[page], cols, DISP_W);
        sim_display_dirty = 1;
    }
}

void board_display_contrast(uint8_t v) { (void)v; }

void board_display_power(int on)
{
    if (sim_display_on != on) {
        sim_display_on = on;
        sim_display_dirty = 1;
    }
}

void board_usb_pullup(int on) { (void)on; }
int board_diag(uint8_t *buf, int max) { (void)buf; (void)max; return 0; }
int board_serial(char *out, int max) { (void)out; (void)max; return 0; }

/* the real keyboard types these as a USB keyboard; the sim only shows them */
void kbd_tap(uint8_t modifier, uint8_t key)
{
    static const struct { uint8_t key; const char *name; } names[] = {
        {KEY_Q, "Q"}, {KEY_R, "R"}, {KEY_ENTER, "Return"}, {KEY_SPACE, "Space"}, {KEY_MINUS, "-"},
        {KEY_EQUAL, "="}, {KEY_RIGHT, "Right"}, {KEY_LEFT, "Left"}, {KEY_DOWN, "Down"}, {KEY_UP, "Up"},
    };
    const char *name = "?";
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].key == key)
            name = names[i].name;
    snprintf(sim_kbd_last, sizeof sim_kbd_last, "%s%s%s%s%s", modifier & KMOD_CTRL ? "Ctrl+" : "",
             modifier & KMOD_ALT ? "Opt+" : "", modifier & KMOD_SHIFT ? "Shift+" : "",
             modifier & KMOD_CMD ? "Cmd+" : "", name);
    sim_kbd_ms = sim_now_ms;
}
