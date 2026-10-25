#pragma once
/* Smart Play: scales, chords (including a learned chord), strum, the
   arpeggiator and note repeat, between the keybed and MIDI out.
   Hardware independent; host tests in test/test_smartplay.c. */
#include <stdint.h>

enum sp_scale_mode { SCALE_OFF, SCALE_QUANTIZE, SCALE_EASY, SCALE_MODE_COUNT };
enum sp_chord { CHORD_OFF, CHORD_OCTAVE, CHORD_FIFTH, CHORD_TRIAD, CHORD_SEVENTH, CHORD_NINTH, CHORD_MEMORY, CHORD_COUNT };
enum sp_arp_mode { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_ORDER, ARP_RANDOM, ARP_CHORD, ARP_MODE_COUNT };
enum sp_arp_rate { RATE_1_4, RATE_1_8, RATE_1_8T, RATE_1_16, RATE_1_16T, RATE_1_32, RATE_COUNT };
enum sp_strum_dir { STRUM_UP, STRUM_DOWN, STRUM_ALTERNATE, STRUM_DIR_COUNT };
enum sp_repeat_vel { REPEAT_VEL_KEY, REPEAT_VEL_STRIP, REPEAT_VEL_COUNT };

#define SP_CHORD_MAX 6   /* notes per chord, including the played note */
#define SP_SCALE_COUNT 18

typedef struct {
    const char *name;
    uint8_t len;
    uint8_t steps[12];  /* semitones from root, ascending */
} sp_scale_t;

extern const sp_scale_t sp_scales[];
extern const uint8_t sp_scale_count;
extern const char *const sp_chord_names[CHORD_COUNT];
extern const char *const sp_arp_mode_names[ARP_MODE_COUNT];
extern const char *const sp_arp_rate_names[RATE_COUNT];

typedef struct {
    uint8_t scale_mode;   /* sp_scale_mode */
    uint8_t scale;        /* index into sp_scales */
    uint8_t root;         /* 0 = C .. 11 = B */
    uint8_t chord;        /* sp_chord; built from scale degrees when a scale is on */
    uint8_t arp_on;
    uint8_t arp_mode;
    uint8_t arp_rate;
    uint8_t arp_octaves;  /* 1..4 */
    uint8_t arp_gate;     /* percent of step, 10..100 */
    uint8_t arp_hold;     /* latch: released keys keep playing until the next new chord */
    uint8_t arp_swing;    /* percent, 50 = straight .. 75 */
    uint16_t bpm_x10;     /* internal tempo, 1200 = 120.0 BPM */
    uint8_t ext_clock;    /* follow incoming MIDI clock instead of bpm */
    uint8_t repeat_on;    /* note repeat: held notes retrigger at arp_rate */
    uint8_t repeat_vel;   /* sp_repeat_vel */
    uint8_t strum_ms;     /* 0 = off; spread of a chord's note-ons */
    uint8_t strum_dir;    /* sp_strum_dir */
    uint8_t chord_mem_n;  /* learned chord: number of notes (0 = none) */
    int8_t chord_mem[SP_CHORD_MAX];  /* semitones from the played key, [0] = 0 */
} sp_config_t;

typedef void (*sp_out_fn)(uint8_t note, uint8_t velocity, int on);

void sp_init(sp_config_t *cfg, sp_out_fn out);
void sp_defaults(sp_config_t *cfg);
/* id identifies the physical key, so a note-off releases what its note-on
   started even if octave, transpose or the config changed in between */
void sp_key(uint8_t id, uint8_t note, uint8_t velocity, int on);
int sp_tick_us(uint32_t elapsed_us);   /* drives the internal clock; returns 24-PPQN ticks elapsed */
void sp_midi_clock(void);              /* 0xF8 */
void sp_midi_start(void);              /* 0xFA */
void sp_midi_continue(void);           /* 0xFB */
void sp_midi_stop(void);               /* 0xFC */
void sp_panic(void);                   /* releases everything sounding and forgets held keys */
void sp_config_changed(void);          /* call after editing cfg while keys may be held */
void sp_set_expression(uint8_t v);     /* 0..127, note repeat velocity from the mod strip */

int sp_engine_on(void);                /* arp or note repeat enabled */
int sp_engine_playing(void);           /* ... and producing notes right now */
int sp_clock_alive(void);              /* the clock that drives the engine is running */
uint8_t sp_beat_tick(void);            /* 0..23 position in the current beat */
int sp_held_count(void);               /* keys physically down */

/* A scale from outside (the DAW's key) instead of cfg's root and type.
   mask bit n = root + n is in the scale; root < 0 goes back to cfg.
   Returns the matching sp_scales index, or -1 (name is kept, not copied). */
int sp_set_external_scale(int root, uint16_t mask, const char *name);
const char *sp_scale_name(void);       /* the scale in use */
int sp_scale_root(void);
uint16_t sp_scale_mask(void);          /* bit n: root + n semitones is in the scale in use */

/* chord learning: notes of all keys down when sp_learn_chord is called */
int sp_learn_chord(void);              /* 0 if at least two notes were held */
