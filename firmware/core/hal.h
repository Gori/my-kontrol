#pragma once
/* The line between the portable core and a keyboard.

   core/       never touches hardware; builds on the host for tests
   platform/   one per MCU family: clocks, flash, watchdog, reboot
   boards/     one per keyboard: pins, matrix, sensors, lights, display

   A new keyboard implements everything below (see docs/porting.md). */
#include <stdint.h>
#include "board_config.h"   /* from boards/<board>/: sizes and flash map */
#include "controls.h"

/* ---------- events: board -> core ---------- */

enum mk_event_type {
    EV_KEY,          /* id = key 0..BOARD_KEYS-1 (lowest = 0), value = velocity, 0 = released */
    EV_BUTTON,       /* id = CTL_*, value = 1 pressed / 0 released */
    EV_ENCODER,      /* 4D encoder turned: value = detents, signed (+ = clockwise) */
    EV_KNOB,         /* id = knob, value = signed steps (+ = clockwise) */
    EV_KNOB_TOUCH,   /* id = knob, value = 1 touched / 0 released */
    EV_STRIP,        /* id = STRIP_PITCH / STRIP_MOD, value = 0..16383, or -1 when released */
    EV_PEDAL,        /* id = 0 tip / 1 ring, value = 1 pressed / 0 released */
};

enum { STRIP_PITCH, STRIP_MOD };

typedef struct {
    uint8_t type, id;
    int16_t value;
} mk_event_t;

/* ---------- board ---------- */

enum {
    CAP_STRIPS = 1 << 0,
    CAP_KNOB_TOUCH = 1 << 1,
    CAP_4D = 1 << 2,
    CAP_PEDAL = 1 << 3,
};

typedef struct {
    const char *name;        /* "Komplete Kontrol M32" */
    const char *short_name;  /* "M32": USB product and About screen */
    uint8_t id;              /* host protocol board ID (docs/protocol.md) */
    uint8_t keys;            /* keybed size */
    uint8_t base_note;       /* MIDI note of key 0 at octave 0 */
    uint8_t knobs;
    uint8_t caps;            /* CAP_* */
} board_info_t;

extern const board_info_t board_info;

void board_early_init(void);       /* display and the pins board_boot_combo needs */
int board_boot_combo(void);        /* 1 if the "stay in bootloader" buttons are held at power-up */
void board_init(void);             /* everything else; called once after the clock is up */
int board_poll(mk_event_t *ev);    /* main loop: 1 and *ev when something happened */
void board_tick(void);             /* every 100 us, from the platform tick interrupt */
void board_light(uint8_t ctl, uint8_t level);   /* button light, 0 = off .. 255 = full */
void board_lights_flush(void);
void board_display_page(int page, const uint8_t *cols);  /* one 8-pixel row band, DISP_W bytes */
void board_display_contrast(uint8_t v);
void board_display_power(int on);
void board_usb_pullup(int on);
int board_diag(uint8_t *buf, int max);  /* board-specific bytes for the diag request */
int board_serial(char *out, int max);   /* USB serial to report, or 0 to use the chip ID */

/* ---------- platform ---------- */

uint32_t hal_ms(void);             /* milliseconds since boot */
void hal_delay_ms(uint32_t ms);    /* busy wait, feeds the watchdog */
void hal_watchdog_feed(void);
int hal_flash_erase(uintptr_t addr);                              /* one page; 0 = ok */
int hal_flash_write(uintptr_t addr, const void *src, uint32_t len);  /* 0 = ok */
void hal_unique_id(uint8_t out[12]);
void hal_usb_init(void);           /* clock and reset the USB peripheral */
void hal_tick_start(void);         /* starts the 100 us tick that calls board_tick() */
void hal_mark_stable(void);        /* ran long enough: clear the crash counter */
void hal_enter_bootloader(void) __attribute__((noreturn));
void hal_reboot(void) __attribute__((noreturn));
