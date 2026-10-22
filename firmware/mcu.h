#pragma once
/* Mackie Control Universal protocol, device side. Logic and Ableton Live both
   support it natively ("Mackie Control" control surface). */
#include <stdint.h>

/* MCU button note numbers (sent as note-on, velocity 127 press / 0 release) */
enum {
    MCU_REC = 0x00, MCU_SOLO = 0x08, MCU_MUTE = 0x10, MCU_SELECT = 0x18, MCU_VPOT_PUSH = 0x20,
    MCU_ASSIGN_TRACK = 0x28, MCU_ASSIGN_SEND, MCU_ASSIGN_PAN, MCU_ASSIGN_PLUGIN, MCU_ASSIGN_EQ, MCU_ASSIGN_INSTR,
    MCU_BANK_LEFT = 0x2E, MCU_BANK_RIGHT, MCU_CH_LEFT, MCU_CH_RIGHT, MCU_FLIP, MCU_GLOBAL,
    MCU_NAME_VALUE = 0x34, MCU_SMPTE_BEATS, MCU_F1 = 0x36,
    MCU_SHIFT = 0x46, MCU_OPTION, MCU_CONTROL, MCU_ALT,
    MCU_READ = 0x4A, MCU_WRITE, MCU_TRIM, MCU_TOUCH, MCU_LATCH, MCU_GROUP,
    MCU_SAVE = 0x50, MCU_UNDO, MCU_CANCEL, MCU_ENTER, MCU_MARKER, MCU_NUDGE, MCU_CYCLE, MCU_DROP,
    MCU_REPLACE, MCU_CLICK, MCU_SOLO_GLOBAL,
    MCU_REWIND = 0x5B, MCU_FFWD, MCU_STOP, MCU_PLAY, MCU_RECORD,
    MCU_UP = 0x60, MCU_DOWN, MCU_LEFT, MCU_RIGHT, MCU_ZOOM, MCU_SCRUB,
    MCU_FADER_TOUCH = 0x68,
};

typedef void (*mcu_send_fn)(const uint8_t *bytes, uint16_t n);

void mcu_init(mcu_send_fn send);
void mcu_button(uint8_t note, int pressed);
void mcu_vpot(uint8_t strip, int delta);       /* strip 0..7 */
void mcu_vpot_touch(uint8_t strip, int touched);
void mcu_jog(int delta);
void mcu_fader(uint8_t strip, uint16_t value); /* 0..16383 */

/* host -> device */
void mcu_sysex(const uint8_t *msg, uint16_t n);  /* complete F0..F7 */
void mcu_note(uint8_t note, uint8_t velocity);   /* LED state */
void mcu_cc(uint8_t cc, uint8_t value);          /* V-pot rings, timecode */

/* state for the UI */
extern char mcu_lcd[2][57];        /* two 56-char rows, NUL terminated */
extern uint8_t mcu_led[128];       /* 0 off, 1 blink, 2 on */
extern uint8_t mcu_ring[8];        /* V-pot ring position 0..11 */
extern char mcu_timecode[11];      /* rightmost digit first as sent; see mcu.c */
extern uint8_t mcu_connected;      /* host has talked to us */
extern uint16_t mcu_lcd_gen;       /* increments on every LCD change */
