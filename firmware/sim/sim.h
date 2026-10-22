#pragma once
/* Simulator internals shared by sim.c, sim_hal.c and sim_midi.c. */
#include <stdint.h>
#include <stdio.h>
#include "hal.h"

/* sim_hal.c: what the core told the "board" */
extern uint32_t sim_now_ms;
extern uint8_t sim_lights[CTL_COUNT];
extern uint8_t sim_display[DISP_H / 8][DISP_W];
extern int sim_display_on;
extern int sim_display_dirty;
extern char sim_kbd_last[32];       /* last USB keyboard tap, as text */
extern uint32_t sim_kbd_ms;

void sim_hal_init(void);
void sim_quit(int code) __attribute__((noreturn));   /* sim.c: restore the terminal and exit */

/* sim_midi.c: CoreMIDI virtual ports */
extern FILE *sim_log;               /* sysex hex log, or 0 */
extern uint32_t sim_midi_in, sim_midi_out;   /* USB-MIDI packets moved */

int sim_midi_open(void);            /* 0 = ok */
void sim_midi_close(void);
void sim_midi_rx(void);             /* main thread: queued input into midi_rx_packet() */
void sim_midi_tx(void);             /* main thread: the core's output queue to the virtual sources */
