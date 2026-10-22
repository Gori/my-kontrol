#pragma once
/* USB-MIDI framing for both directions, independent of the USB stack.

   Out: complete messages are packed into 4-byte USB-MIDI packets and queued
   (1 KB); the USB layer drains the queue. A message that does not fit is
   dropped whole, so a full queue never leaves half a sysex on the wire.
   In: packets are unpacked into per-port byte parsers (running status,
   sysex up to MIDI_SYSEX_MAX, realtime bytes anywhere). */
#include <stdint.h>

enum { PORT_KEYS, PORT_DAW, PORT_COUNT };

#define MIDI_SYSEX_MAX 160

int midi_send(uint8_t port, const uint8_t *msg, uint16_t n);   /* one message; 0 = queued */
int midi_send3(uint8_t port, uint8_t status, uint8_t a, uint8_t b);
int midi_tx_free(void);                    /* packets of room left */
int midi_tx_peek(uint8_t pkt[4]);          /* 1 if a packet is waiting */
void midi_tx_pop(void);
extern uint16_t midi_tx_dropped;

void midi_rx_packet(const uint8_t pkt[4]);
void midi_rx_reset(void);

/* supplied by the application */
void app_midi_message(uint8_t port, uint8_t status, uint8_t d0, uint8_t d1);
void app_midi_sysex(uint8_t port, const uint8_t *msg, uint16_t n);   /* F0 .. F7 */
void app_midi_realtime(uint8_t port, uint8_t b);
