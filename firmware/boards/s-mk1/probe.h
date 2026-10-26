#pragma once
/* S MK1 bring-up probe (hybrid developer builds, MK_PROBE): the raw panel state out as
   SysEx, a hex view on the displays, and single LED bits, for mapping controls and lights
   on a keyboard. probe.c has the message format. */
#include <stdint.h>

/* from the drivers' threads: latest state only */
void probe_inputs(uint32_t bits);                       /* the 32 input bits, 1 = pressed */
void probe_sensor(int which, const uint8_t *r, int n);  /* 0 = 0x12 (knobs), 1 = 0x15 (touch) */
void probe_keybed(uint8_t b);                           /* each byte from the keybed UART */

/* main loop */
void probe_task(void);
int probe_midi_in(const uint8_t pkt[4]);   /* 1 = a probe command, taken */
int probe_display(void);                   /* 1 = the displays show the probe, not the UI */
