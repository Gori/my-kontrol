#pragma once
/* My Kontrol host protocol, device side (docs/protocol.md). A DAW script
   or a DAW's own surface support drives the views. */
#include <stdint.h>

/* knob modes: the standard ones, then 4 the host may define (MODES) */
enum host_view { VIEW_DEVICE, VIEW_VOLUME, VIEW_CLIPS, VIEW_PAN, VIEW_SENDS, VIEW_INSTRUMENT, VIEW_COUNT,
                 VIEW_HOST = 16, VIEW_HOST_END = 20 };

enum {
    HT_TRACK = 0x00, HT_DEVICE = 0x01, HT_BANK = 0x02, HT_SUBTITLE = 0x03, HT_POSITION = 0x04,
    HT_KNOB_NAME = 0x10, HT_KNOB_VALUE = 0x18,
    HT_COL = 0x30, HT_ROW = 0x38,
};

enum { HOST_TF_PLAYING = 1, HOST_TF_ARR_REC = 2, HOST_TF_SESSION_REC = 4, HOST_TF_LOOP = 8,
       HOST_TF_METRO = 16, HOST_TF_CAPTURE = 32 };

/* HELLO features (v2) */
enum { HF_PARAMS = 1, HF_METERS = 2, HF_SCALE = 4, HF_KEYS = 8, HF_LISTS = 16, HF_MESSAGE = 32,
       HF_COLOURS = 64, HF_GRID = 128 };

enum { SLOT_EMPTY, SLOT_CONTINUOUS, SLOT_TOGGLE, SLOT_ENUM };

enum { GRID_NONE, GRID_EMPTY, GRID_STOPPED, GRID_PLAYING, GRID_RECORDING, GRID_QUEUED,
       GRID_QUEUED_REC, GRID_ARMED };

enum { ROW_FOLDER = 1, ROW_PLAYING = 2, ROW_CHECKED = 4, ROW_DISABLED = 8 };
enum { MSG_LAST = 1, MSG_BUSY = 2 };

#define HOST_TEXT_LEN 25
#define HOST_NAME_LEN 17
#define GRID_COLS 8
#define GRID_ROWS 4
#define HOST_MODES 10
#define HOST_MODE_NAME 11
#define HOST_KEYS 16           /* key labels held (IDENT key_labels) */
#define HOST_KEY_CHARS 10      /* characters kept of each (IDENT label_chars) */
#define HOST_LIST_ROWS 3       /* rows on screen (IDENT list_rows) */
#define HOST_LIST_CACHE 6      /* rows held (IDENT list_cache) */
#define HOST_ROW_CHARS 24
#define HOST_MSG_CHARS 160     /* IDENT msg_chars */
#define HOST_ROW_NONE 0xFFFF

typedef struct {
    uint16_t index;            /* item in the list, HOST_ROW_NONE = unused */
    uint8_t flags;             /* ROW_* */
    char text[HOST_ROW_CHARS + 1];
} host_row_t;

typedef struct {
    uint8_t connected;
    uint8_t lost;              /* silent for 3 s: state kept, marked lost */
    uint8_t proto;             /* from HELLO */
    uint16_t features;         /* HF_*, v2 only */
    char name[HOST_NAME_LEN];
    uint8_t view;
    char track[HOST_TEXT_LEN], device[HOST_TEXT_LEN], bank[HOST_TEXT_LEN], subtitle[HOST_TEXT_LEN];
    char knob_name[8][HOST_TEXT_LEN / 2 + 1], knob_value[8][HOST_TEXT_LEN / 2 + 1];
    uint8_t knob_pos[8], knob_flags[8];           /* 0..127; bit 0 mapped, 1 bipolar, 2 stepped */
    char col[GRID_COLS][9], row[GRID_ROWS][13];
    uint8_t light[64];         /* by CTL_*, LIGHT states */
    uint8_t transport, beat, beats_per_bar;
    uint16_t tempo_x10;
    uint8_t grid_cols, grid_rows, cursor_col, cursor_row;
    uint8_t grid[GRID_ROWS][GRID_COLS];

    /* v2 */
    uint8_t n_modes, mode_id[HOST_MODES], mode_flags[HOST_MODES];
    char mode_name[HOST_MODES][HOST_MODE_NAME];
    uint8_t modes_gen;         /* increments on every MODES */
    uint8_t set_mode;          /* SET_MODE request: mode id + 1, 0 = none */
    uint8_t page_gen, page_bank, page_banks;
    uint8_t slot_kind[8], slot_steps[8];
    char group[8][9];
    uint8_t meter[8], meter_kind;                 /* slot meters, METER target 0 */
    uint8_t channel_meter[2], channel_meter_kind; /* selected channel, target 1 */
    uint8_t scale_state, scale_root, scale_gen;
    uint16_t scale_mask;
    char scale_name[13];
    uint8_t keys_n, key_note[HOST_KEYS];
    char key_label[HOST_KEYS][HOST_KEY_CHARS + 1];
    uint8_t list_id;
    uint16_t list_total, list_cursor, list_first;
    char list_title[HOST_TEXT_LEN];
    host_row_t rows[HOST_LIST_CACHE];
    uint8_t msg_id, msg_flags;
    uint8_t msg_len;
    char msg[HOST_MSG_CHARS + 1];

    uint16_t gen;              /* increments on every change that affects the screen */
    uint16_t beat_gen;         /* increments on every beat */
} host_state_t;

extern host_state_t host;

typedef void (*host_send_fn)(const uint8_t *bytes, uint16_t n);
typedef void (*host_notify_fn)(const char *title, const char *value);

void host_init(host_send_fn send, host_notify_fn notify);
void host_tick_ms(uint32_t ms);        /* HI announcements, the silence timeout, DESCRIPTION parts */
void host_usb_reset(void);             /* USB went away */
int host_sysex(const uint8_t *m, uint16_t n);   /* 1 if it was ours */

void host_button(uint8_t ctl, int down);
void host_note(uint8_t note, int on);  /* channel 16 note-on, 127 / 0 */
void host_knob(uint8_t knob, int delta);
void host_knob_touch(uint8_t knob, int touched);
void host_encoder(int delta);
void host_set_view(uint8_t view);      /* also tells the host (VIEW), even if unchanged */

/* MODES: index of view in the host's list, or -1; a host that sent no
   MODES (v1) offers every standard view */
int host_mode_index(uint8_t view);
int host_mode_enabled(uint8_t view);
const char *host_mode_name(uint8_t view);   /* the host's title, or 0 */
int host_modal(void);                   /* a list or message screen is open */
const char *host_key_label(uint8_t note);   /* label for a note, or 0 */

/* string decoding (docs/protocol.md "Strings"), shared with the tests:
   reads one length-prefixed string at d[*pos], advances *pos; folds
   characters the fonts lack. Returns 0 if the data ran out. */
int host_string(const uint8_t *d, int len, int *pos, char *dst, int size, int keep_newline);

/* the board's surface description (JSON, ASCII), from boards/<board>/surface.json */
extern const char board_surface_json[];
