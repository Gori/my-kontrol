#pragma once
/* Application: routes board events to Smart Play, the DAW links and the UI. */
#include <stdint.h>
#include "hal.h"

enum link { LINK_NONE, LINK_MACKIE, LINK_HOST };

void app_init(void);
void app_event(const mk_event_t *ev);
void app_poll(void);               /* once per main-loop pass */
void app_usb_mounted(int mounted);

int app_link(void);                /* which DAW protocol is in charge */
int app_shift(void);
uint8_t app_knob_value(int page, int knob);
int app_learning(void);            /* chord learn in progress (Ideas held) */
void app_settings_changed(void);   /* call after editing settings */
void app_panic(void);
void app_learn_chord(void);        /* store the chord currently held */
void app_enter_bootloader(void);
