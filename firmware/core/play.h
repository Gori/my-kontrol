#pragma once
/* Keys to MIDI: velocity curves, octave/transpose, keyboard split, Smart
   Play for the upper zone, touch strips and pedals, with every note-on
   matched by exactly one note-off (per-key latching and voice counting). */
#include <stdint.h>

void play_init(void);
void play_key(uint8_t key, uint8_t velocity, int on);   /* key = keybed index */
void play_strip(uint8_t strip, int16_t value);           /* value 0..16383, -1 released */
void play_pedal(uint8_t sw, int down);
void play_pedal_unplugged(void);
void play_tick_ms(uint32_t ms);
void play_settings_changed(void);  /* releases notes on a channel or split change */
void play_panic(void);             /* everything off, including CC 123 on the channels in use */
uint8_t play_channel(void);        /* main (upper zone) channel currently in use */
int play_keys_down(void);

extern uint8_t play_last_note, play_last_velocity;
extern uint16_t play_note_gen;     /* increments on every played note-on */
