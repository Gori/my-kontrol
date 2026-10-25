#pragma once
/* A display backend. core/ui.c fills a screen_t (screen.h) and calls
   ui_renderer.show() once per frame; the renderer turns that one model into
   output and owns everything about how it looks. A renderer must: clear its
   display and draw the base screen named by s->kind; then, if s->lost is set,
   overlay the "connection lost" banner; then, if s->popup.show is set, overlay
   the popup last (it sits on top of everything). It reads only the screen_t it
   is given plus board_info, never host/settings/app state, so one model drives
   any display. For SCR_MESSAGE it may clamp *s->u.message.scroll to its own
   line count. Exactly one renderer is linked per board (the Makefile picks the
   source): ui/oled.c for 128x32 pixels, a later ui/seg9.c for the S-series. A
   new backend is added by providing ui_renderer; core/ui.c does not change. */
#include "screen.h"

typedef struct {
    void (*show)(const screen_t *s);
} renderer_t;

/* provided by the one renderer source linked for this board */
extern const renderer_t ui_renderer;
