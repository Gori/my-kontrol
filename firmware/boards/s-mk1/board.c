/* Komplete Kontrol S-Series MK1: the board side of core/hal.h, on XS1 tile 0.
   Displays work (display.c). Not yet: keybed, buttons, encoder, knobs and touch (sensor
   MCUs), pedals, button lights, light guide, strip LEDs, display backlight; their hooks are
   empty and board_poll() reports nothing. */
#include "hal.h"
#include "display.h"

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
    return 0;
}

void board_init(void)
{
}

int board_poll(mk_event_t *ev)
{
    (void)ev;
    return 0;
}

void board_tick(void)
{
}

void board_seg_show(const seg_display_t *d)
{
    display_show(d);
}

void board_light(uint8_t ctl, uint8_t level) { (void)ctl, (void)level; }
void board_lights_flush(void) {}
void board_key_light(uint8_t key, uint8_t r, uint8_t g, uint8_t b) { (void)key, (void)r, (void)g, (void)b; }
void board_key_lights_flush(void) {}
void board_strip_leds(uint8_t strip, uint8_t style, uint16_t value) { (void)strip, (void)style, (void)value; }
void board_display_contrast(uint8_t v) { (void)v; }
void board_display_power(int on) { (void)on; }
void board_usb_pullup(int on) { (void)on; }
int board_diag(uint8_t *buf, int max) { (void)buf, (void)max; return 0; }
int board_serial(char *out, int max) { (void)out, (void)max; return 0; }
