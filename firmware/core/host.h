#pragma once
/* My Kontrol host protocol, device side (docs/protocol.md). A DAW script
   (the Ableton Live script in daw/ableton/) drives the views. */
#include <stdint.h>

enum host_view { VIEW_DEVICE, VIEW_MIXER, VIEW_CLIPS, VIEW_COUNT };

enum {
    HT_TRACK = 0x00, HT_DEVICE = 0x01, HT_BANK = 0x02, HT_SUBTITLE = 0x03,
    HT_KNOB_NAME = 0x10, HT_KNOB_VALUE = 0x18,
    HT_COL = 0x30, HT_ROW = 0x38,
};

enum { HOST_TF_PLAYING = 1, HOST_TF_ARR_REC = 2, HOST_TF_SESSION_REC = 4, HOST_TF_LOOP = 8,
       HOST_TF_METRO = 16, HOST_TF_CAPTURE = 32 };

enum { GRID_NONE, GRID_EMPTY, GRID_STOPPED, GRID_PLAYING, GRID_RECORDING, GRID_QUEUED,
       GRID_QUEUED_REC, GRID_ARMED };

#define HOST_TEXT_LEN 25
#define GRID_COLS 8
#define GRID_ROWS 4

typedef struct {
    uint8_t connected;
    uint8_t view;
    char track[HOST_TEXT_LEN], device[HOST_TEXT_LEN], bank[HOST_TEXT_LEN], subtitle[HOST_TEXT_LEN];
    char knob_name[8][HOST_TEXT_LEN / 2 + 1], knob_value[8][HOST_TEXT_LEN / 2 + 1];
    uint8_t knob_pos[8], knob_flags[8];
    char col[GRID_COLS][9], row[GRID_ROWS][13];
    uint8_t light[64];             /* by CTL_*, LIGHT states */
    uint8_t transport, beat, beats_per_bar;
    uint16_t tempo_x10;
    uint8_t grid_cols, grid_rows, cursor_col, cursor_row;
    uint8_t grid[GRID_ROWS][GRID_COLS];
    uint16_t gen;                  /* increments on every change that affects the screen */
    uint16_t beat_gen;             /* increments on every beat */
} host_state_t;

extern host_state_t host;

typedef void (*host_send_fn)(const uint8_t *bytes, uint16_t n);
typedef void (*host_notify_fn)(const char *title, const char *value);

void host_init(host_send_fn send, host_notify_fn notify);
void host_tick_ms(uint32_t ms);        /* HI announcements and the silence timeout */
void host_usb_reset(void);             /* USB went away */
int host_sysex(const uint8_t *m, uint16_t n);   /* 1 if it was ours */

void host_button(uint8_t ctl, int down);
void host_knob(uint8_t knob, int delta);
void host_knob_touch(uint8_t knob, int touched);
void host_encoder(int delta);
void host_set_view(uint8_t view);
