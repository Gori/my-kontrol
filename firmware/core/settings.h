#pragma once
/* Everything the user can change; persisted by settings_store.c.
   Changing this struct: bump SETTINGS_VERSION in settings_store.c and keep
   settings_sanitize() in step. */
#include <stdint.h>
#include "hal.h"
#include "smartplay.h"

#define KNOB_PAGES 4

enum vel_curve { VEL_SOFT, VEL_LINEAR, VEL_HARD, VEL_FIXED, VEL_CURVE_COUNT };
enum daw_mode { DAW_AUTO, DAW_OFF, DAW_MODE_COUNT };
enum knob_mode { KNOBS_DAW, KNOBS_MIDI, KNOB_MODE_COUNT };
enum tempo_light { TEMPO_LIGHT_AUTO, TEMPO_LIGHT_ALWAYS, TEMPO_LIGHT_OFF, TEMPO_LIGHT_COUNT };

typedef struct {
    sp_config_t sp;
    uint8_t channel;        /* 0..15 */
    int8_t octave;          /* -3..+3 */
    int8_t transpose;       /* -12..+12 */
    uint8_t vel_curve;      /* vel_curve */
    uint8_t fixed_velocity; /* used when vel_curve == VEL_FIXED */
    uint8_t vel_curve_prev; /* restored when fixed velocity is toggled off */

    uint8_t split_on;       /* keyboard split: keys below split_note go to the lower zone */
    uint8_t split_note;     /* MIDI note of the first upper-zone key at octave 0 */
    uint8_t lower_channel;
    int8_t lower_octave;

    uint8_t knob_cc[KNOB_PAGES][BOARD_KNOBS];
    uint8_t knob_page;
    uint8_t knob_mode;      /* knob_mode */
    uint8_t pedal_cc[2];    /* tip, ring */
    uint8_t strip_mod_cc;   /* mod strip CC */

    uint8_t daw_mode;       /* daw_mode */
    uint8_t tempo_light;    /* tempo_light */
    uint8_t clock_out;      /* send MIDI clock on the Keys port from the internal tempo */
    uint8_t contrast;       /* OLED contrast */
    uint8_t led_dim;        /* brightness of available-but-off button lights */
    uint8_t screensaver;    /* index into settings_screensaver_min */
    uint8_t reserved[8];    /* zero; room for small additions without a version bump */
} settings_t;

extern settings_t settings;
extern const uint8_t settings_screensaver_min[];   /* 0 = never */
extern const uint8_t settings_screensaver_count;

void settings_defaults(settings_t *s);
void settings_sanitize(settings_t *s);   /* clamp everything into range */
