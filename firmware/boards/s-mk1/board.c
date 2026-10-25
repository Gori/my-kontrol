/* Komplete Kontrol S-Series MK1: the board side of core/hal.h, on XS1 tile 0.
   Threads: the core (main), the 100 us tick (panel.c: input registers), the sensor MCUs
   (panel.c), the keybed UART (keybed.c), two for the lights (lights.c). The displays (display.c) are sent
   from the main loop. Still TODO: the control <-> input bit and light bit maps, the touch
   node map, knob resolution, strip LEDs (see panel.c and lights.c). */
#include "hal.h"
#include "display.h"
#include "panel.h"

const board_info_t board_info = {
    .name = "Komplete Kontrol S49",
    .short_name = "S49",
    .id = 2,
    .keys = BOARD_KEYS,
    .base_note = BOARD_BASE_NOTE,
    .knobs = BOARD_KNOBS,
    .caps = CAP_STRIPS | CAP_KNOB_TOUCH | CAP_4D | CAP_PEDAL | CAP_LIGHT_GUIDE |
            CAP_STRIP_LEDS | CAP_ENCODER_TOUCH | CAP_EXPRESSION,
};

void board_early_init(void)
{
    display_init();
}

int board_boot_combo(void)
{
    return 0;                           /* TODO: a power-up combination for update mode */
}

void board_init(void)
{
    panel_init();
    sensors_start();
    keybed_start();
    lights_start();
}

int board_poll(mk_event_t *ev)
{
    return evq_pop(&keybed_events, ev) || evq_pop(&panel_events, ev) || evq_pop(&sensor_events, ev);
}

void board_tick(void)
{
    panel_tick();
}

void board_seg_show(const seg_display_t *d)
{
    display_show(d);
}

void board_light(uint8_t ctl, uint8_t level) { lights_button(ctl, level); }
void board_lights_flush(void) {}
void board_key_light(uint8_t key, uint8_t r, uint8_t g, uint8_t b) { lights_key(key, r, g, b); }
void board_key_lights_flush(void) {}
void board_strip_leds(uint8_t strip, uint8_t style, uint16_t value) { lights_strip(strip, style, value); }

static uint8_t bl_level = 127, bl_on = 1;
void board_display_contrast(uint8_t v) { bl_level = v >> 1; lights_backlight(bl_on ? bl_level : 0); }
void board_display_power(int on) { bl_on = on != 0; lights_backlight(bl_on ? bl_level : 0); }
void board_usb_pullup(int on) { (void)on; }

/* the 32 raw input bits (for mapping the buttons), sensor status, dropped events */
int board_diag(uint8_t *buf, int max)
{
    int n = panel_diag(buf, max);
    if (n + 2 <= max) {
        buf[n++] = (uint8_t)panel_dropped;
        buf[n++] = (uint8_t)(panel_dropped >> 8);
    }
    return n;
}

int board_serial(char *out, int max) { (void)out, (void)max; return 0; }
