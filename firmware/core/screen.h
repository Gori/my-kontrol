#pragma once
/* The screen model: what the UI wants shown, with no idea how it is drawn.
   core/ui.c decides the content and fills a screen_t, then hands it to the
   active renderer (render.h). ui/oled.c draws it on the M32's 128x32 pixels; a
   later ui/seg9.c draws it on the S-series' 9-segment displays. A screen the
   model cannot express is a model bug, not a reason to draw from a board. */
#include <stdint.h>
#include "hal.h"       /* BOARD_KNOBS, DISP_W, DISP_H */
#include "host.h"      /* GRID_ROWS, GRID_COLS, HOST_LIST_ROWS */

enum screen_kind {
    SCR_SPLASH,     /* boot banner: two centred lines */
    SCR_PLAY,       /* home: a status line, a big centre line, velocity, a hint */
    SCR_CCPAGE,     /* home knob page: one vertical CC bar per knob */
    SCR_KNOBS,      /* the control section: a title, a right field, 8 knob cells */
    SCR_CLIPS,      /* the host's clip grid */
    SCR_MENU,       /* a settings menu */
    SCR_LIST,       /* the host's browser / take list */
    SCR_MESSAGE,    /* the host's wrapped, scrolling message */
    SCR_DIALOG,     /* a modal: confirm, learn, about or update */
};

/* dialog ids (screen_t.u.dialog.id) */
enum { SCR_DLG_CONFIRM = 1, SCR_DLG_LEARN, SCR_DLG_ABOUT, SCR_DLG_UPDATE };

/* the bar under a knob cell */
enum { BAR_NONE, BAR_DOTTED, BAR_FILL };

#define SCR_MENU_ROWS 3

/* one knob of the control section: a name (a slice, name_len characters), a
   value text (for the popup and the S-series), and a bar. the bar is bar/bar_max
   so the renderer, not the UI, picks the pixels; bipolar fills from the centre.
   meter is an optional 0..127 level line, 0 = none. */
typedef struct {
    const char *name;
    uint8_t name_len;
    const char *value;
    uint8_t bar_style;              /* BAR_* */
    uint8_t bar, bar_max, bipolar;
    uint8_t meter;
} knob_slot_t;

typedef struct {
    uint8_t kind;                   /* enum screen_kind */

    /* overlays, drawn over any base screen */
    uint8_t lost;                   /* the host went quiet: a banner over the top line */
    struct {
        uint8_t show;
        const char *title, *value;
    } popup;

    union {
        struct { const char *a, *b; } splash;

        struct {
            const char *status;     /* "CH1 OCT+1 ..." */
            const char *centre;     /* scale, the last note, or the product name */
            uint8_t has_velocity, velocity;
            const char *vel_text;   /* the velocity readout */
            const char *hint;       /* bottom line */
        } play;

        struct {
            const char *title;
            uint8_t n;
            struct { uint8_t value; const char *cc; } knob[BOARD_KNOBS];
        } ccpage;

        struct {
            const char *title;
            const char *right_a, *right_b;  /* top-right text (a, then a space and b) */
            uint8_t room_limit;             /* fit right_a/right_b to the width left of them */
            knob_slot_t slot[8];
            uint8_t channel_meter;          /* right-edge level, 0 = none */
        } knobs;

        struct {
            const char *row_label;
            uint8_t row_label_len;
            const char *col_label;          /* 0 = none */
            uint8_t rows, cols, cr, cc, blink;
            uint8_t cell[GRID_ROWS][GRID_COLS];
        } clips;

        struct {
            const char *title;
            int count, top, sel;
            uint8_t editing, nrow;
            struct { const char *name, *value; } row[SCR_MENU_ROWS];
        } menu;

        struct {
            const char *title;
            const char *count;      /* "3/42", drawn if it fits */
            uint16_t total, cursor, first;
            struct { uint8_t present, flags; const char *text; } row[HOST_LIST_ROWS];
        } list;

        struct {
            const char *text;
            int *scroll;            /* the renderer clamps it to the line count */
            uint8_t busy_dot;
        } message;

        struct {
            uint8_t id;             /* SCR_DLG_* */
            const char *confirm;    /* the question (confirm) */
            const char *about;      /* version + board (about) */
            uint8_t blink;
        } dialog;
    } u;
} screen_t;
