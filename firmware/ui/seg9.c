/* The S-series renderer: the screen model (core/screen.h) on 9 segment displays of
   2 lines x 8 characters with a bar each (core/hal.h, seg_display_t). It builds one frame
   of text and hands it to board_seg_show(); the board's driver owns the segments.

   Layout. Display 1 (left of the knobs) carries the title and a subtitle; displays 2..9
   (under knobs 1..8) carry one knob each: name, value, bar. Screens without a knob per
   display flow their words over the 9 displays, a word never split between two displays:
   - play (home):  line 1 the status, the velocity at the right of display 9 (its bar);
                   line 2 the centre text, then the hint
   - CC page:      display 1 title; displays 2..9 "CC<n>", the value, the value as a bar
   - knobs:        display 1 title / device; displays 2..9 name / value / bar
   - clips:        display 1 row / column name; displays 2..9 one column each: the cursor
                   row and the row below, '>' marking the cursor
   - menu, list:   display 1 title / position; displays 2..9 the selected row (line 1)
                   and the next one (line 2); a menu value being edited shows as [value]
   - message:      2 lines of the flowed text from the scroll position (clamped here)
   - dialogs:      the text on the two lines
   - lost:         "CONNECTION LOST" over line 1 of displays 1-2
   - popup:        title on line 1 and value on line 2 of displays 1-2, last
   A '.' after a character lights that character's dot instead of taking a cell. */
#include "render.h"

#define NDISP 9
#define COLS (NDISP * SEG_CHARS)

static seg_display_t f[NDISP];

static void clear_line(int line, int d0, int d1)
{
    for (int d = d0; d <= d1; d++) {
        for (int c = 0; c < SEG_CHARS; c++)
            f[d].line[line][c] = ' ';
        f[d].dots[line] = 0;
    }
}

/* visible length of s (at most n bytes): a dot after a character takes no cell */
static int width(const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n && s && s[i]; i++)
        if (!(s[i] == '.' && i > 0 && s[i - 1] != '.'))
            w++;
    return w;
}

/* write up to n bytes of s on a line from column col (0..71) up to column end */
static void put_n(int line, int col, int end, const char *s, int n)
{
    if (!s)
        return;
    int first = col;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] == '.' && i > 0 && s[i - 1] != '.' && col > first) {
            f[(col - 1) / SEG_CHARS].dots[line] |= 1 << ((col - 1) % SEG_CHARS);
            continue;
        }
        if (col > end || col >= COLS)
            break;
        f[col / SEG_CHARS].line[line][col % SEG_CHARS] = s[i];
        col++;
    }
}

static void put(int line, int col, int end, const char *s)
{
    put_n(line, col, end, s, 255);
}

static void put_right(int line, int end, const char *s)
{
    int w = width(s, 255);
    put(line, end + 1 - w, end, s);
}

/* Word flow: the words of s (n bytes) on a line from display d0 to d1, a word never split
   between displays unless it is longer than one; line < 0 only measures. Stops at a newline
   or when the displays are full; returns the bytes used, *more = 1 if text is left. */
static int flow(int line, int d0, int d1, const char *s, int n, int *more)
{
    int d = d0, pos = 0, i = 0;
    *more = 0;
    while (i < n && s[i]) {
        if (s[i] == '\n') {
            i++;
            break;
        }
        if (s[i] == ' ') {
            i++;
            continue;
        }
        int j = i;
        while (j < n && s[j] && s[j] != ' ' && s[j] != '\n')
            j++;
        int w = width(s + i, j - i);
        if (pos && pos + 1 + w > SEG_CHARS) {        /* on to the next display */
            d++;
            pos = 0;
        } else if (pos) {
            pos++;
        }
        if (d > d1) {
            *more = 1;
            return i;
        }
        while (w > 0) {                              /* longer than a display: split */
            int room = SEG_CHARS - pos, k = i, vis = 0;
            while (k < j && (vis < room || (s[k] == '.' && k > i && s[k - 1] != '.'))) {
                if (!(s[k] == '.' && k > i && s[k - 1] != '.'))
                    vis++;
                k++;
            }
            if (line >= 0)
                put_n(line, d * SEG_CHARS + pos, d * SEG_CHARS + 7, s + i, k - i);
            pos += vis;
            w -= vis;
            i = k;
            if (w > 0) {
                d++;
                pos = 0;
                if (d > d1) {
                    *more = 1;
                    return i;
                }
            }
        }
        i = j;
    }
    while (i < n && s[i] == ' ')
        i++;
    *more = i < n && s[i];
    return i;
}

static int flow_all(int line, int d0, int d1, const char *s)
{
    int more;
    return s ? flow(line, d0, d1, s, 255, &more) : 0;
}

/* a text on one display: line 1, and what does not fit (from the last space) on line 2 */
static void two_lines(int d, const char *s)
{
    int n = 0, cut = -1;
    while (s && s[n])
        n++;
    if (width(s, n) <= SEG_CHARS) {
        put(0, d * SEG_CHARS, d * SEG_CHARS + 7, s);
        return;
    }
    for (int i = 0; i < n && i <= SEG_CHARS; i++)
        if (s[i] == ' ')
            cut = i;
    if (cut < 0)
        cut = SEG_CHARS;
    put_n(0, d * SEG_CHARS, d * SEG_CHARS + 7, s, cut);
    while (s[cut] == ' ')
        cut++;
    put(1, d * SEG_CHARS, d * SEG_CHARS + 7, s + cut);
}

static char *fmt_int(char *p, int v)
{
    char t[8];
    int n = 0;
    if (v < 0)
        *p++ = '-', v = -v;
    do t[n++] = '0' + v % 10; while (v /= 10);
    while (n)
        *p++ = t[--n];
    *p = 0;
    return p;
}

/* the display after the last one used on a line from d0 (d0 if none) */
static int next_free(int line, int d0)
{
    int d = NDISP;
    while (d > d0) {
        int used = 0;
        for (int c = 0; c < SEG_CHARS; c++)
            used |= f[d - 1].line[line][c] != ' ';
        if (used)
            break;
        d--;
    }
    return d;
}

static void draw_play(const screen_t *s)
{
    flow_all(0, 0, NDISP - 2, s->u.play.status);
    flow_all(1, 0, NDISP - 1, s->u.play.centre);
    flow_all(1, next_free(1, 0), NDISP - 1, s->u.play.hint);
    if (s->u.play.has_velocity) {
        put_right(0, COLS - 1, s->u.play.vel_text);
        f[NDISP - 1].meter = s->u.play.velocity * 2 + (s->u.play.velocity > 0);
    }
}

static void draw_ccpage(const screen_t *s)
{
    two_lines(0, s->u.ccpage.title);
    for (int k = 0; k < s->u.ccpage.n && k < NDISP - 1; k++) {
        int c = (k + 1) * SEG_CHARS;
        char b[12];
        b[0] = 'C', b[1] = 'C';
        int i = 2;
        for (const char *q = s->u.ccpage.knob[k].cc; *q && i < 10; )
            b[i++] = *q++;
        b[i] = 0;
        put(0, c, c + 7, b);
        int v = s->u.ccpage.knob[k].value;
        fmt_int(b, v);
        put_right(1, c + 7, b);
        f[k + 1].meter = v * 2 + (v > 0);
    }
}

static void knob_slot(int d, const knob_slot_t *sl)
{
    int c = d * SEG_CHARS;
    put_n(0, c, c + 7, sl->name, sl->name_len < SEG_CHARS ? sl->name_len : SEG_CHARS);
    put(1, c, c + 7, sl->value);
    if (sl->bar_style == BAR_FILL && sl->bar_max)
        f[d].meter = (uint8_t)((sl->bar > sl->bar_max ? sl->bar_max : sl->bar) * 255u / sl->bar_max);
    else if (sl->meter)
        f[d].meter = sl->meter * 2 + 1;
}

static void draw_knobs(const screen_t *s)
{
    put(0, 0, 7, s->u.knobs.title);
    put(1, 0, 7, s->u.knobs.right_a);
    const char *b = s->u.knobs.right_b;
    int used = width(s->u.knobs.right_a, 255);
    if (b && b[0] && used + 1 + width(b, 255) <= SEG_CHARS)
        put(1, used + 1, 7, b);
    if (s->u.knobs.channel_meter)
        f[0].meter = s->u.knobs.channel_meter * 2 + 1;
    for (int k = 0; k < 8; k++)
        knob_slot(k + 1, &s->u.knobs.slot[k]);
}

static const char *clip_state(uint8_t st, uint8_t blink)
{
    switch (st) {
    case GRID_ARMED: return "ARM";
    case GRID_STOPPED: return "STOP";
    case GRID_PLAYING: return "PLAY";
    case GRID_RECORDING: return blink ? "REC" : "";
    case GRID_QUEUED: return blink ? "QUEUE" : "";
    case GRID_QUEUED_REC: return blink ? "Q REC" : "";
    }
    return "";
}

static void draw_clips(const screen_t *s)
{
    put_n(0, 0, 7, s->u.clips.row_label, s->u.clips.row_label_len);
    put(1, 0, 7, s->u.clips.col_label);
    int cr = s->u.clips.cr;
    for (int c = 0; c < s->u.clips.cols && c < NDISP - 1; c++)
        for (int l = 0; l < 2 && cr + l < s->u.clips.rows; l++) {
            int col = (c + 1) * SEG_CHARS;
            if (l == 0 && c == s->u.clips.cc)
                put(0, col, col, ">");
            put(l, col + 1, col + 7, clip_state(s->u.clips.cell[cr + l][c], s->u.clips.blink));
        }
}

/* a menu or list row on one line of displays 2..9: the mark and the name flowed over
   displays 2..8, the value (if any) at the right of display 9 */
static void row(int line, char mark, const char *name, const char *value, int bracket)
{
    char b[48], *p = b;
    *p++ = mark;
    for (const char *q = name; q && *q && p < b + 46; ) *p++ = *q++;
    *p = 0;
    int last = value && value[0] ? NDISP - 2 : NDISP - 1;
    flow_all(line, 1, last, mark == ' ' ? b + 1 : b);
    if (value && value[0]) {
        p = b;
        if (bracket) *p++ = '[';
        for (const char *q = value; *q && p < b + 45; ) *p++ = *q++;
        if (bracket) *p++ = ']';
        *p = 0;
        put_right(line, COLS - 1, b);
    }
}

static void draw_menu(const screen_t *s)
{
    char b[16];
    two_lines(0, s->u.menu.title);
    if (width(s->u.menu.title, 255) <= SEG_CHARS && s->u.menu.count) {
        char *p = fmt_int(b, s->u.menu.sel + 1);
        *p++ = '/';
        fmt_int(p, s->u.menu.count);
        put(1, 0, 7, b);
    }
    int r = s->u.menu.sel - s->u.menu.top;
    for (int l = 0; l < 2 && r + l < s->u.menu.nrow; l++)
        row(l, l == 0 ? '>' : ' ', s->u.menu.row[r + l].name, s->u.menu.row[r + l].value,
            l == 0 && s->u.menu.editing);
}

static void draw_list(const screen_t *s)
{
    put(0, 0, 7, s->u.list.title);
    if (s->u.list.total && s->u.list.count)
        put(1, 0, 7, s->u.list.count);
    if (!s->u.list.total) {
        put(0, SEG_CHARS, COLS - 1, "EMPTY");
        return;
    }
    int r = s->u.list.cursor - s->u.list.first;
    for (int l = 0; l < 2 && r + l >= 0 && r + l < HOST_LIST_ROWS; l++) {
        if (s->u.list.first + r + l >= s->u.list.total)
            break;
        uint8_t present = s->u.list.row[r + l].present, fl = s->u.list.row[r + l].flags;
        char mark = !present ? ' ' : fl & ROW_PLAYING ? '>' : fl & ROW_CHECKED ? '+' : l == 0 ? '-' : ' ';
        const char *t = present ? s->u.list.row[r + l].text : "...";
        row(l, mark, t, present && (fl & ROW_FOLDER) ? "/" : "", 0);
    }
}

/* the host's message: words flowed over the 9 displays, line by line, 2 lines from the
   scroll position (clamped here to the line count) */
static void draw_message(const screen_t *sc)
{
    const char *s = sc->u.message.text;
    int *scroll = sc->u.message.scroll;
    int lines = 0, i = 0, more;
    while (s[i]) {
        i += flow(-1, 0, NDISP - 1, s + i, 4096, &more);
        lines++;
    }
    int max = lines > 2 ? lines - 2 : 0;
    if (*scroll > max)
        *scroll = max;
    if (*scroll < 0)
        *scroll = 0;
    i = 0;
    for (int l = 0; s[i] && l < *scroll + 2; l++)
        i += flow(l >= *scroll ? l - *scroll : -1, 0, NDISP - 1, s + i, 4096, &more);
    if (sc->u.message.busy_dot)
        f[NDISP - 1].dots[0] |= 0x80;
}

static void draw_dialog(const screen_t *s)
{
    switch (s->u.dialog.id) {
    case SCR_DLG_CONFIRM:
        flow_all(0, 0, NDISP - 1, s->u.dialog.confirm);
        flow_all(1, 0, NDISP - 1, "PUSH = YES");
        break;
    case SCR_DLG_LEARN:
        flow_all(0, 0, NDISP - 1, "HOLD A CHORD, THEN PUSH");
        flow_all(1, 0, NDISP - 1, s->u.dialog.blink ? "* LEARN *" : "LEARN");
        break;
    case SCR_DLG_ABOUT:
        flow_all(0, 0, NDISP - 1, "MY KONTROL OPEN SOURCE GPL-3.0");
        flow_all(1, 0, NDISP - 1, s->u.dialog.about);
        break;
    case SCR_DLG_UPDATE:
        flow_all(0, 0, NDISP - 1, "UPDATE MODE");
        flow_all(1, 0, NDISP - 1, "READY FOR THE WEB INSTALLER");
        break;
    }
}

static void seg9_show(const screen_t *s)
{
    for (int d = 0; d < NDISP; d++) {
        clear_line(0, d, d);
        clear_line(1, d, d);
        f[d].meter = 0;
    }
    switch (s->kind) {
    case SCR_SPLASH:
        two_lines(0, s->u.splash.a);
        flow_all(0, 1, NDISP - 1, s->u.splash.b);
        break;
    case SCR_PLAY:    draw_play(s); break;
    case SCR_CCPAGE:  draw_ccpage(s); break;
    case SCR_KNOBS:   draw_knobs(s); break;
    case SCR_CLIPS:   draw_clips(s); break;
    case SCR_MENU:    draw_menu(s); break;
    case SCR_LIST:    draw_list(s); break;
    case SCR_MESSAGE: draw_message(s); break;
    case SCR_DIALOG:  draw_dialog(s); break;
    }
    if (s->lost) {
        clear_line(0, 0, 1);
        flow_all(0, 0, 1, "CONNECTION LOST");
    }
    if (s->popup.show) {
        clear_line(0, 0, 1);
        clear_line(1, 0, 1);
        f[0].meter = f[1].meter = 0;
        flow_all(0, 0, 1, s->popup.title);
        flow_all(1, 0, 1, s->popup.value);
    }
    board_seg_show(f);
}

const renderer_t ui_renderer = { seg9_show };
