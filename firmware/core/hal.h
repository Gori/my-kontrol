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
    EV_AFTERTOUCH,   /* keybed pressure, value = 0..127 (CAP_AFTERTOUCH) */
    EV_EXPRESSION,   /* expression pedal, value = 0..16383 (CAP_EXPRESSION) */
};

enum { STRIP_PITCH, STRIP_MOD };

typedef struct {
    uint8_t type, id;
    int16_t value;
} mk_event_t;

/* ---------- board ---------- */

/* What the keyboard has; a core feature runs only when its cap is set.
   Bits 0..3 are IDENT's caps byte (docs/protocol.md). */
enum {
    CAP_STRIPS = 1 << 0,
    CAP_KNOB_TOUCH = 1 << 1,
    CAP_4D = 1 << 2,
    CAP_PEDAL = 1 << 3,
    CAP_LIGHT_GUIDE = 1 << 4,    /* an RGB light per key: board_key_light */
    CAP_STRIP_LEDS = 1 << 5,     /* lights beside the touch strips: board_strip_leds */
    CAP_AFTERTOUCH = 1 << 6,     /* EV_AFTERTOUCH */
    CAP_EXPRESSION = 1 << 7,     /* EV_EXPRESSION */
    CAP_ENCODER_TOUCH = 1 << 8,  /* CTL_ENCODER_TOUCH */
};

/* board_strip_leds styles */
enum { STRIP_LEDS_OFF, STRIP_LEDS_BAR, STRIP_LEDS_DOT, STRIP_LEDS_CENTRE };

typedef struct {
    const char *name;        /* "Komplete Kontrol M32" */
    const char *short_name;  /* "M32": USB product and About screen */
    uint8_t id;              /* host protocol board ID (docs/protocol.md) */
    uint8_t keys;            /* keybed size */
    uint8_t base_note;       /* MIDI note of key 0 at octave 0 */
    uint8_t knobs;
    uint16_t caps;           /* CAP_* */
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

/* Segment text displays (the S-series: 9 displays of 2 lines x 8 characters). A board with
   BOARD_SEG_DISPLAYS > 0 (board_config.h) links ui/seg9.c instead of ui/oled.c; it hands
   the board every frame as text, and the board's driver turns characters into segments.
   Such a board never gets board_display_page; pixel boards never get board_seg_show. */
#ifndef BOARD_SEG_DISPLAYS
#define BOARD_SEG_DISPLAYS 0
#endif
#define SEG_CHARS 8
typedef struct {
    char line[2][SEG_CHARS];   /* ' ' = blank; a character the font lacks shows blank */
    uint8_t dots[2];           /* bit c: the dot after character c (0..6; the 8th has none) */
    uint8_t meter;             /* the bar above the text: 0 = off, 1..255 = 1..9 blocks */
} seg_display_t;
void board_seg_show(const seg_display_t *d);   /* BOARD_SEG_DISPLAYS displays: draw them */

/* Only called on boards with the cap, so other boards leave them out. */
void board_key_light(uint8_t key, uint8_t r, uint8_t g, uint8_t b);   /* CAP_LIGHT_GUIDE, 0..255 each */
void board_key_lights_flush(void);
void board_strip_leds(uint8_t strip, uint8_t style, uint16_t value);  /* CAP_STRIP_LEDS: STRIP_LEDS_*, 0..16383 */

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
/* Optional: a platform that defines HAL_HAS_IDLE (platform.mk) gets hal_idle() once per
   main-loop pass, to wait for input or the next millisecond instead of spinning. */
void hal_idle(void);
void hal_enter_bootloader(void) __attribute__((noreturn));
void hal_reboot(void) __attribute__((noreturn));
