/* The 128x32 OLED renderer: it turns a screen model (core/screen.h) into pixels
   in gfx_fb. This is the M32's drawing, moved out of core/ui.c; gfx.c and the
   fonts are its tools. It reads only the screen_t it is handed, never the host
   or settings state, so the same model can later drive ui/seg9.c. */
#include "render.h"
#include "gfx.h"

/* a left title, inverted, as the control-section and list screens draw it;
   returns the x where the rest of the top line may start */
static int title_badge(const char *name)
{
    int w = gfx_text(1, 0, name, &font_small);
    gfx_invert(0, 0, w + 1, 8);
    return w + 4;
}

static void draw_splash(const screen_t *s)
{
    gfx_text_center(0, s->u.splash.a, &font_huge);
    gfx_text_center(DISP_H - 8, s->u.splash.b, &font_small);
}

static void draw_play(const screen_t *s)
{
    gfx_text(0, 0, s->u.play.status, &font_small);
    gfx_text(0, 9, s->u.play.centre, &font_large);
    if (s->u.play.has_velocity) {
        int h = s->u.play.velocity * 22 / 127;
        gfx_fill(DISP_W - 3, 31 - h, 3, h + 1, 1);
        gfx_text_right(DISP_W - 5, 17, s->u.play.vel_text, &font_small);
    }
    gfx_text_n(0, 25, s->u.play.hint, 25, &font_small);
}

static void draw_ccpage(const screen_t *s)
{
    gfx_text(0, 0, s->u.ccpage.title, &font_small);
    int n = s->u.ccpage.n, w = DISP_W / n;
    for (int k = 0; k < n; k++) {
        int x = k * w, v = s->u.ccpage.knob[k].value;
        int h = v * 14 / 127;
        gfx_frame(x + 2, 9, w - 4, 16);
        gfx_fill(x + 3, 24 - h, w - 6, h, 1);
        const char *cc = s->u.ccpage.knob[k].cc;
        gfx_text((x + w / 2) - gfx_text_width(cc, &font_small) / 2, 25, cc, &font_small);
    }
}

/* 8 knobs in two rows of four: name, position bar, and (meters) a level line */
static void knob_cells(const knob_slot_t *sl)
{
    for (int k = 0; k < 8; k++) {
        int x = (k & 3) * 32, y = k < 4 ? 8 : 20;
        gfx_text_n(x, y, sl[k].name, sl[k].name_len, &font_small);
        switch (sl[k].bar_style) {
        case BAR_FILL:   gfx_bar(x, y + 8, 29, 3, sl[k].bar, sl[k].bar_max, sl[k].bipolar); break;
        case BAR_DOTTED: gfx_hline_dotted(x, y + 9, 29); break;
        }
        if (sl[k].meter)
            gfx_fill(x, y + 11, (sl[k].meter * 29 + 126) / 127, 1, 1);
    }
}

static void draw_knobs(const screen_t *s)
{
    int x = title_badge(s->u.knobs.title);
    if (s->u.knobs.room_limit) {              /* fit a device / extra pair */
        char b[26], *p = b;
        int room = (DISP_W - x) / 5;
        const char *d = s->u.knobs.right_a, *extra = s->u.knobs.right_b;
        while (*d && p - b < room) *p++ = *d++;
        if (extra[0] && p - b + 2 < room) {
            *p++ = ' ';
            for (d = extra; *d && p - b < room; ) *p++ = *d++;
        }
        *p = 0;
        gfx_text_right(DISP_W, 0, b, &font_small);
    } else if (s->u.knobs.right_a) {
        gfx_text_right(DISP_W, 0, s->u.knobs.right_a, &font_small);
    }
    knob_cells(s->u.knobs.slot);
    if (s->u.knobs.channel_meter) {
        int h = s->u.knobs.channel_meter * 23 / 127;
        if (h)
            gfx_fill(DISP_W - 2, 31 - h, 2, h + 1, 1);
    }
}

static void draw_clips(const screen_t *s)
{
    gfx_text_n(0, 0, s->u.clips.row_label, s->u.clips.row_label_len, &font_small);
    if (s->u.clips.col_label)
        gfx_text_right(DISP_W, 0, s->u.clips.col_label, &font_small);
    int cr = s->u.clips.cr, cc = s->u.clips.cc;
    for (int r = 0; r < s->u.clips.rows; r++)
        for (int c = 0; c < s->u.clips.cols; c++) {
            int x = c * 16 + 2, y = 9 + r * 6, st = s->u.clips.cell[r][c];
            switch (st) {
            case GRID_EMPTY: gfx_pixel(x + 6, y + 1, 1); break;
            case GRID_ARMED: gfx_fill(x + 4, y + 1, 5, 1, 1); break;
            case GRID_STOPPED: gfx_frame(x + 1, y, 10, 4); break;
            case GRID_PLAYING: gfx_fill(x + 1, y, 10, 4, 1); break;
            case GRID_RECORDING: case GRID_QUEUED: case GRID_QUEUED_REC:
                if (s->u.clips.blink) gfx_fill(x + 1, y, 10, 4, 1); else gfx_frame(x + 1, y, 10, 4);
                break;
            }
            if (r == cr && c == cc)
                gfx_frame(x - 1, y - 1, 14, 6);
        }
}

static void draw_menu(const screen_t *s)
{
    int tx = gfx_text(1, 0, s->u.menu.title, &font_small);
    gfx_fill(tx + 3, 3, DISP_W - 3 - tx - 3, 1, 1);
    int n = s->u.menu.count;
    if (n > SCR_MENU_ROWS) {
        int h = 24 * SCR_MENU_ROWS / n, y = 8 + (24 - h) * s->u.menu.top / (n - SCR_MENU_ROWS);
        gfx_fill(DISP_W - 2, y, 2, h, 1);
    }
    for (int r = 0; r < s->u.menu.nrow; r++) {
        int i = s->u.menu.top + r, y = 8 + r * 8;
        const char *name = s->u.menu.row[r].name, *v = s->u.menu.row[r].value;
        int w = gfx_text_width(v, &font_small);
        gfx_text_n(2, y, name, (DISP_W - 8 - w) / 5, &font_small);
        gfx_text(DISP_W - 5 - w, y, v, &font_small);
        if (i == s->u.menu.sel) {
            if (s->u.menu.editing)
                gfx_invert(DISP_W - 6 - w, y, w + 2, 8);
            else
                gfx_invert(0, y, DISP_W - 3, 8);
        }
    }
}

static void draw_list(const screen_t *s)
{
    int x = title_badge(s->u.list.title);
    if (s->u.list.total && s->u.list.count &&
        gfx_text_width(s->u.list.count, &font_small) < DISP_W - x)
        gfx_text_right(DISP_W, 0, s->u.list.count, &font_small);
    if (!s->u.list.total) {
        gfx_text(2, 12, "Empty", &font_small);
        return;
    }
    for (int r = 0; r < HOST_LIST_ROWS; r++) {
        uint16_t i = s->u.list.first + r;
        if (i >= s->u.list.total)
            break;
        int y = 8 + r * 8;
        uint8_t present = s->u.list.row[r].present, flags = s->u.list.row[r].flags;
        char b[26], *p = b;
        *p++ = !present ? ' ' : flags & ROW_PLAYING ? '>' : flags & ROW_CHECKED ? '+' : ' ';
        const char *t = present ? s->u.list.row[r].text : "...";
        while (*t && p - b < 23) *p++ = *t++;
        *p = 0;
        gfx_text(1, y, b, &font_small);
        if (present && (flags & ROW_FOLDER))
            gfx_text_right(DISP_W - 1, y, "/", &font_small);
        if (present && (flags & ROW_DISABLED))
            gfx_hline_dotted(6, y + 4, gfx_text_width(b, &font_small) - 6);
        if (i == s->u.list.cursor)
            gfx_invert(0, y, DISP_W, 8);
    }
}

/* the host's message, word-wrapped, 4 lines from the scroll position */
#define MSG_COLS (DISP_W / 5)

static int wrap_line(const char *s, int i, int *len)
{
    while (s[i] == ' ') i++;
    int n = 0, cut = -1;
    while (s[i + n] && s[i + n] != '\n' && n < MSG_COLS) {
        if (s[i + n] == ' ') cut = n;
        n++;
    }
    int next = i + n;
    if (s[next] == '\n') next++;
    else if (s[next] && s[next] != ' ' && cut > 0) next = i + cut, n = cut;
    *len = n;
    return next;
}

static void draw_message(const screen_t *sc)
{
    const char *s = sc->u.message.text;
    int *scroll = sc->u.message.scroll;
    int lines = 0, i = 0, len;
    while (s[i]) { i = wrap_line(s, i, &len); lines++; }
    int max = lines > 4 ? lines - 4 : 0;
    if (*scroll > max) *scroll = max;
    i = 0;
    for (int l = 0; s[i] && l < *scroll + 4; l++) {
        int start = i;
        while (s[start] == ' ') start++;
        i = wrap_line(s, i, &len);
        if (l >= *scroll)
            gfx_text_n(0, (l - *scroll) * 8, s + start, len, &font_small);
    }
    if (max) {                          /* scroll position */
        int h = 32 * 4 / lines, y = (32 - h) * *scroll / max;
        gfx_fill(DISP_W - 1, y, 1, h, 1);
    }
    if (sc->u.message.busy_dot)
        gfx_fill(DISP_W - 4, 0, 3, 3, 1);
}

static void draw_dialog(const screen_t *s)
{
    switch (s->u.dialog.id) {
    case SCR_DLG_CONFIRM:
        gfx_text_center(2, s->u.dialog.confirm, &font_small);
        gfx_text_center(14, "Push = Yes", &font_large);
        break;
    case SCR_DLG_LEARN:
        gfx_text_center(1, "Hold a chord,", &font_small);
        gfx_text_center(10, "then push", &font_small);
        gfx_text_center(22, s->u.dialog.blink ? "* Learn *" : "  Learn  ", &font_small);
        break;
    case SCR_DLG_ABOUT:
        gfx_text_center(0, "My Kontrol", &font_large);
        gfx_text_center(16, s->u.dialog.about, &font_small);
        gfx_text_center(24, "Open source, GPL-3.0", &font_small);
        break;
    case SCR_DLG_UPDATE:
        gfx_text_center(0, "Update Mode", &font_large);
        gfx_text_center(18, "Ready for the", &font_small);
        gfx_text_center(25, "web installer", &font_small);
        break;
    }
}

static void draw_lost(void)
{
    gfx_fill(0, 0, DISP_W, 8, 0);
    gfx_text_center(0, "Connection lost", &font_small);
    gfx_invert(0, 0, DISP_W, 8);
}

static void draw_popup(const screen_t *s)
{
    const char *title = s->popup.title, *value = s->popup.value;
    gfx_fill(0, 0, DISP_W, DISP_H, 0);
    gfx_frame(0, 0, DISP_W, DISP_H);
    gfx_text_center(1, title, &font_small);
    int n = 0;
    while (value[n]) n++;
    if (n * font_huge.w <= DISP_W - 8)
        gfx_text_center(8, value, &font_huge);
    else if (n * font_large.w <= DISP_W - 8)
        gfx_text_center(11, value, &font_large);
    else
        gfx_text_center(16, value, &font_small);
}

static void oled_show(const screen_t *s)
{
    gfx_clear();
    switch (s->kind) {
    case SCR_SPLASH:  draw_splash(s); break;
    case SCR_PLAY:    draw_play(s); break;
    case SCR_CCPAGE:  draw_ccpage(s); break;
    case SCR_KNOBS:   draw_knobs(s); break;
    case SCR_CLIPS:   draw_clips(s); break;
    case SCR_MENU:    draw_menu(s); break;
    case SCR_LIST:    draw_list(s); break;
    case SCR_MESSAGE: draw_message(s); break;
    case SCR_DIALOG:  draw_dialog(s); break;
    }
    if (s->lost)
        draw_lost();
    if (s->popup.show)
        draw_popup(s);
}

const renderer_t ui_renderer = { oled_show };
