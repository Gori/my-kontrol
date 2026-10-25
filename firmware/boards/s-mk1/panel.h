#pragma once
/* The S MK1 panel inputs and outputs (tile 0). Events reach board_poll() through queues
   that each have one producer thread. */
#include "hal.h"

typedef struct {
    volatile uint32_t buf[32];
    volatile uint8_t head, tail;
} evq_t;

int evq_push(evq_t *q, uint8_t type, uint8_t id, int16_t value);   /* 0 = full, dropped */
int evq_pop(evq_t *q, mk_event_t *ev);

/* panel.c: buttons, encoder and pedal switches (input registers, every 1 ms from
   board_tick()) and the sensor MCUs (knobs, touch, strips, expression pedal, every 10 ms,
   their own thread) */
extern evq_t panel_events, sensor_events;
extern uint16_t panel_dropped;
void panel_init(void);
int panel_boot_combo(void);        /* both page buttons held (before panel_init) */
void panel_tick(void);              /* every 100 us */
void sensors_start(void);
int panel_diag(uint8_t *buf, int max);

/* keybed.c: the keybed scanner's UART, on its own thread */
extern evq_t keybed_events;
void keybed_start(void);

/* lights.c: the three LED chains and the display backlight, on two threads */
void lights_start(void);
void lights_key(uint8_t key, uint8_t r, uint8_t g, uint8_t b);   /* 0..255 */
void lights_button(uint8_t ctl, uint8_t level);
void lights_strip(uint8_t strip, uint8_t style, uint16_t value);
void lights_backlight(uint8_t level);                            /* 0..127 */
