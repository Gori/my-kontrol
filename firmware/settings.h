#pragma once
/* Everything the user can change; persisted to flash by settings_store.c. */
#include <stdint.h>
#include "smartplay.h"

enum vel_curve { VEL_LINEAR, VEL_SOFT, VEL_HARD, VEL_FIXED, VEL_CURVE_COUNT };

typedef struct {
    sp_config_t sp;
    uint8_t channel;        /* 0..15 */
    int8_t octave;          /* -3..+3 */
    int8_t transpose;       /* -12..+12 */
    uint8_t vel_curve;      /* vel_curve */
    uint8_t fixed_velocity; /* used when vel_curve == VEL_FIXED */
    uint8_t knob_cc[4][8];  /* 4 pages */
    uint8_t knob_page;      /* 0..3 */
    uint8_t pedal_cc[2];    /* tip, ring switch */
    uint8_t strip_mod_cc;   /* mod strip CC, default 1 */
    uint8_t daw_mode;       /* 0 = MIDI only, 1 = Mackie Control */
    uint8_t contrast;       /* OLED contrast */
} settings_t;

extern settings_t settings;
void settings_defaults(settings_t *s);
