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
enum { DAW_YES, DAW_NO };            /* daw_scale, daw_view: 0 = the DAW may set it */
enum { NAV_OFF, NAV_AUTO, NAV_ALWAYS, NAV_COUNT };   /* nav_keys */
enum { MIDI_OUT_BOTH, MIDI_OUT_USB, MIDI_OUT_DIN, MIDI_OUT_COUNT };   /* midi_out: where the keys go */

/* What the knobs control while a DAW is connected; M / S step through
   knob_modes[] (KM_OFF entries are skipped). */
enum knob_target { KM_OFF, KM_INSTRUMENT, KM_PLUGIN, KM_VOLUME, KM_PAN, KM_SENDS, KM_COUNT };
#define KNOB_MODE_SLOTS 5

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
    uint8_t knob_modes[KNOB_MODE_SLOTS];   /* order of the M / S cycle */
    uint8_t knob_target;    /* current knob_target, remembered */
    uint8_t pedal_cc[2];    /* tip, ring */
    uint8_t strip_mod_cc;   /* mod strip CC */

    uint8_t daw_mode;       /* daw_mode */
    uint8_t tempo_light;    /* tempo_light */
    uint8_t clock_out;      /* send MIDI clock to the keys route from the internal tempo */
    uint8_t contrast;       /* OLED contrast */
    uint8_t led_dim;        /* brightness of available-but-off button lights */
    uint8_t screensaver;    /* index into settings_screensaver_min */
    uint8_t nav_keys;       /* NAV_*: with a DAW, the 4D types arrow keys / Return (Auto: unless the host takes the 4D) */
    uint8_t daw_scale;      /* DAW_YES: Smart Play follows the DAW's key (SCALE) */
    uint8_t daw_view;       /* DAW_YES: the DAW may switch the knob mode (SET_MODE) */
    uint8_t midi_out;       /* MIDI_OUT_* (BOARD_MIDI_DIN) */
    uint8_t midi_thru;      /* 5-pin in echoed to 5-pin out (BOARD_MIDI_DIN) */
    uint8_t reserved[4];    /* zero; room for small additions without a version bump */
} settings_t;

extern settings_t settings;
extern const uint8_t settings_screensaver_min[];   /* 0 = never */
extern const uint8_t settings_screensaver_count;
extern const char *const knob_target_names[KM_COUNT];

void settings_defaults(settings_t *s);
void settings_sanitize(settings_t *s);   /* clamp everything into range */
