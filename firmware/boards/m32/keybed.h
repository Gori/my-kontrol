#pragma once
#include <stdint.h>
#include "hal.h"

void keybed_pins(void);
void keybed_init(void);
int keybed_boot_combo(void);
void keybed_tick(void);              /* every 100 us, from the platform tick interrupt */
int keybed_next(mk_event_t *ev);     /* 1 if an event was returned */
extern uint16_t keybed_dropped;
