#pragma once
/* 74HC595-style LED chains with 7-bit brightness by bit-angle (binary code) modulation:
   one period is 7 latched frames ("planes"), plane k shown for 2^k time units, so a light
   with level v (0..127) is on for v of the 127 units. 7 shifts per period instead of 128.

   Chain bit i is byte i/8, bit i%8 of the chain's buffer; bytes are shifted out last byte
   first, each byte LSB first (docs/hardware/s-mk1.md, Lights). Outputs are active low.
   The driver is pure: the board shifts the bytes it returns and latches. */
#include <stdint.h>

#define SHIFT595_PLANES 7

/* the bytes to shift for plane k (0..6) of a chain of nbits (a multiple of 8): out[j] is
   the j-th byte to send, a light is on (bit 0) when bit k of its level is set */
void shift595_plane(const uint8_t *level, int nbits, int k, uint8_t *out);
