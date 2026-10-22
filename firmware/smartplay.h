#pragma once
/* Smart Play: scale, chord and arpeggiator processing between the keybed and
   MIDI out. Hardware independent; host tests in test/test_smartplay.c. */
#include <stdint.h>

enum sp_scale_mode { SCALE_OFF, SCALE_QUANTIZE, SCALE_EASY };
enum sp_chord { CHORD_OFF, CHORD_OCTAVE, CHORD_FIFTH, CHORD_TRIAD, CHORD_SEVENTH, CHORD_NINTH, CHORD_COUNT };
enum sp_arp_mode { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_ORDER, ARP_RANDOM, ARP_CHORD, ARP_MODE_COUNT };
enum sp_arp_rate { RATE_1_4, RATE_1_8, RATE_1_8T, RATE_1_16, RATE_1_16T, RATE_1_32, RATE_COUNT };

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
    uint8_t chord;        /* sp_chord; chords are built from scale degrees when a scale is on */
    uint8_t arp_on;
    uint8_t arp_mode;
    uint8_t arp_rate;
    uint8_t arp_octaves;  /* 1..4 */
    uint8_t arp_gate;     /* percent of step, 10..100 */
    uint8_t arp_hold;     /* latch: keys released keep playing until next key-down */
    uint8_t arp_swing;    /* percent, 50 = straight .. 75 */
    uint16_t bpm_x10;     /* internal tempo, e.g. 1200 = 120.0 BPM */
    uint8_t ext_clock;    /* follow incoming MIDI clock instead of bpm */
} sp_config_t;

typedef void (*sp_out_fn)(uint8_t note, uint8_t velocity, int on);

void sp_init(sp_config_t *cfg, sp_out_fn out);
void sp_defaults(sp_config_t *cfg);
void sp_key(uint8_t key, uint8_t velocity, int on);  /* key = raw MIDI note from keybed */
void sp_tick_us(uint32_t elapsed_us);                /* call often; drives the internal clock */
void sp_midi_clock(void);                            /* 0xF8 received */
void sp_midi_start(void);                            /* 0xFA / 0xFB */
void sp_midi_stop(void);                             /* 0xFC */
void sp_panic(void);                                 /* releases everything sounding */
void sp_config_changed(void);                        /* call after editing cfg while keys may be held */
