#pragma once
/* USB-MIDI framing for both directions, independent of the USB stack, and
   the 5-pin DIN jacks on boards that have them (BOARD_MIDI_DIN).

   Out: complete messages are packed into 4-byte USB-MIDI packets and queued
   (1 KB); the USB layer drains the queue. A message that does not fit is
   dropped whole, so a full queue never leaves half a sysex on the wire.
   In: packets are unpacked into per-port byte parsers (running status,
   sysex up to MIDI_SYSEX_MAX, realtime bytes anywhere).
   5-pin: messages for PORT_DIN go into a byte queue of their own, with
   running status for channel messages; the board drains it a byte at a
   time and feeds received bytes back through the same parsers. Sysex on
   EXT and DIN is passed on byte by byte (any length); nothing but realtime
   bytes goes out the 5-pin inside it. */
#include <stdint.h>

/* KEYS, DAW and EXT are USB cables 0, 1 and 2. EXT ("My Kontrol MIDI") is
   the computer's view of the 5-pin jacks; DIN is the jacks themselves. Only
   DIN boards have EXT. */
enum { PORT_KEYS, PORT_DAW, PORT_EXT, PORT_DIN, PORT_COUNT };

#define MIDI_SYSEX_MAX 160

int midi_send(uint8_t port, const uint8_t *msg, uint16_t n);   /* one message; 0 = queued */
int midi_send3(uint8_t port, uint8_t status, uint8_t a, uint8_t b);
uint8_t midi_msg_len(uint8_t status);      /* bytes in a message with this status, sysex 0 */
int midi_tx_free(void);                    /* packets of room left */
int midi_tx_peek(uint8_t pkt[4]);          /* 1 if a packet is waiting */
void midi_tx_pop(void);
extern uint16_t midi_tx_dropped;

/* What the player makes (notes, CCs, bends, pedals, knob CCs, clock) goes to
   the keys route: USB PORT_KEYS and / or PORT_DIN, set by the application. */
enum { MIDI_TO_USB = 1, MIDI_TO_DIN = 2 };
void midi_keys_route(uint8_t route);       /* MIDI_TO_*; MIDI_TO_USB until set */
int midi_keys(const uint8_t *msg, uint16_t n);
int midi_keys3(uint8_t status, uint8_t a, uint8_t b);

int midi_rx_packet(const uint8_t pkt[4]);   /* 0: not now (EXT queue full), offer it again */
void midi_rx_reset(void);

/* 5-pin jacks (BOARD_MIDI_DIN), both from the main loop */
int midi_din_tx_byte(void);                /* next byte for the UART, -1 when empty */
void midi_din_rx_byte(uint8_t b);          /* a byte from the UART, parsed as PORT_DIN */
extern uint16_t midi_din_dropped;

/* one byte of a sysex (F0 .. F7) from port src, out as it comes: to the 5-pin,
   or to a USB cable. Cut short with F7 when the queue is full. */
int midi_din_sysex(uint8_t src, uint8_t b);
int midi_usb_sysex(uint8_t port, uint8_t b);
void midi_din_tick_ms(uint32_t ms);        /* moves EXT on to the 5-pin; ends a sysex that stopped coming */

/* supplied by the application */
void app_midi_message(uint8_t port, uint8_t status, uint8_t d0, uint8_t d1);
void app_midi_sysex(uint8_t port, const uint8_t *msg, uint16_t n);   /* F0 .. F7 */
void app_midi_realtime(uint8_t port, uint8_t b);
void app_midi_sysex_byte(uint8_t port, uint8_t b);   /* EXT and DIN: sysex byte by byte, ends with F7 */
