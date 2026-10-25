#pragma once
/* The S MK1 keybed scanner's UART messages (docs/hardware/s-mk1.md, Keybed): 3 bytes,
   `0x90 | v_hi` (on) or `0x80 | v_hi` (off), the note, `v_lo`; velocity is 10 bits,
   v_hi << 7 | v_lo. Pure: frames from bytes, and the velocity curve. */
#include <stdint.h>

typedef struct {
    uint8_t b[3], n;
} ni_keybed_t;

/* feed one byte; 1 when it completed a message: *note, *on, *raw (10-bit velocity) */
int ni_keybed_byte(ni_keybed_t *k, uint8_t b, uint8_t *note, uint8_t *on, uint16_t *raw);

/* MIDI velocity 1..127 from the 10-bit value; smaller values are faster key strokes */
uint8_t ni_keybed_velocity(uint16_t raw);
