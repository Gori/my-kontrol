/* Host tests for the S-series display stack: drivers/font16.c, drivers/st7565_seg.c (the
   segment RAM map) and ui/seg9.c (screen model -> 9 displays). */
#include <stdio.h>
#include <string.h>
#include "font16.h"
#include "render.h"
#include "st7565_seg.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

static seg_display_t shown[9];
static int shows;

void board_seg_show(const seg_display_t *d)
{
    memcpy(shown, d, sizeof shown);
    shows++;
}

/* ---------- font ---------- */

static void test_font(void)
{
    const char *set = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (const char *a = set; *a; a++) {
        CHECK(font16(*a), "'%c' has no glyph", *a);
        for (const char *b = a + 1; *b; b++)
            CHECK(font16(*a) != font16(*b), "'%c' and '%c' look the same", *a, *b);
    }
    CHECK(font16('a') == font16('A'), "lower case shows as upper case");
    CHECK(font16(' ') == 0 && font16('.') == 0, "space and dot are blank");
    CHECK(font16('~') == 0 && font16(0x7f) == 0, "no glyph: blank");
    CHECK(font16('O') == (SEG_A1 | SEG_A2 | SEG_B | SEG_C | SEG_D1 | SEG_D2 | SEG_E | SEG_F),
          "O is the outer ring");
}

/* ---------- segment RAM map ---------- */

static st7565_seg_ram_t ram;
static uint16_t owner[3][240][8];   /* which feature lit each RAM bit, 0 = none */

static void blank(seg_display_t d[9])
{
    memset(d, 0, 9 * sizeof *d);
    for (int i = 0; i < 9; i++)
        memset(d[i].line, ' ', sizeof d[i].line);
}

/* claim every RAM bit lit by d for a feature; returns how many, -1 on a clash */
static int claim(const seg_display_t d[9], int id)
{
    int n = 0, clash = 0;
    st7565_seg_encode(ram, d);
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 240; i++)
            for (int b = 0; b < 8; b++)
                if (ram[c][i] >> b & 1) {
                    if (owner[c][i][b] && owner[c][i][b] != id)
                        clash = 1;
                    owner[c][i][b] = (uint16_t)id;
                    n++;
                }
    return clash ? -1 : n;
}

/* every segment of every character, every dot and every bar segment of the 9 displays
   lands on its own RAM bit: 9 x (2 x 8 x 16 + 14 dots + 19 meter) = 2601 bits, none shared */
static void test_map_unique(void)
{
    seg_display_t d[9];
    memset(owner, 0, sizeof owner);
    int id = 0, total = 0;
    for (int disp = 0; disp < 9; disp++) {
        for (int line = 0; line < 2; line++)
            for (int pos = 0; pos < 8; pos++) {
                id++;
                blank(d);
                d[disp].line[line][pos] = '8';         /* outer ring + middle: 10 segments */
                int a = claim(d, id);
                d[disp].line[line][pos] = '*';         /* centre bars + diagonals + middle: 8 */
                int b = claim(d, id);
                d[disp].line[line][pos] = '8';
                st7565_seg_encode(ram, d);
                int n = 0;
                for (int c = 0; c < 3; c++)
                    for (int i = 0; i < 240; i++)
                        for (int k = 0; k < 8; k++)
                            n += owner[c][i][k] == id;
                CHECK(a == 10 && b == 8 && n == 16, "display %d line %d char %d: %d+%d bits, %d distinct",
                      disp, line, pos, a, b, n);
                total += n;
            }
        for (int k = 0; k < 16; k++) {
            if (k % 8 == 7)
                continue;                              /* no dot after the 8th character */
            id++;
            blank(d);
            d[disp].dots[k / 8] = 1 << (k % 8);
            CHECK(claim(d, id) == 1, "display %d dot %d", disp, k);
            total++;
        }
        id++;
        blank(d);
        d[disp].meter = 255;                           /* box, 9 lines, 9 blocks */
        CHECK(claim(d, id) == 19, "display %d full meter", disp);
        total += 19;
        for (int k = 1; k <= 9; k++) {
            blank(d);
            d[disp].meter = (uint8_t)(k * 255 / 9);
            st7565_seg_encode(ram, d);
            int n = 0;
            for (int c = 0; c < 3; c++)
                for (int i = 0; i < 240; i++)
                    for (int b = 0; b < 8; b++)
                        n += ram[c][i] >> b & 1;
            CHECK(n == 10 + k, "display %d meter %d/9 lights %d", disp, k, n);
        }
    }
    CHECK(total == 9 * (256 + 14 + 19), "%d features", total);
}

/* spot checks against docs/hardware/s-mk1.md / xmos.md §11.3 */
static void test_map_spots(void)
{
    seg_display_t d[9];
    memset(d, 0, sizeof d);
    for (int i = 0; i < 9; i++)
        memset(d[i].line, ' ', sizeof d[i].line);
    d[0].line[0][0] = '-';                  /* G1 G2: line 1 bits 11/12 -> bytes 1.5 and 4.6 */
    st7565_seg_encode(ram, d);
    CHECK(ram[0][1] == 0x20 && ram[0][4] == 0x40, "line-1 '-' at 1.5 and 4.6 (got %02x %02x)", ram[0][1], ram[0][4]);
    d[0].line[0][0] = ' ';
    d[4].line[1][7] = '_';                  /* D1 D2 of the last line-2 char: base 5q+1, +1.0 +2.0 */
    st7565_seg_encode(ram, d);
    int base = 5 * (8 * 1 + 7) + 1;
    CHECK(ram[1][base + 1] == 1 && ram[1][base + 2] == 1, "last line-2 '_' (got %02x %02x)",
          ram[1][base + 1], ram[1][base + 2]);
    d[4].line[1][7] = ' ';
    d[8].dots[0] = 0x40;                    /* line-1 dot of char 6, slot 2: 5(16+6)+4 bit 3 */
    st7565_seg_encode(ram, d);
    CHECK(ram[2][5 * 22 + 4] == 8, "line-1 dot of char 7");
    d[8].dots[0] = 0;
    d[8].meter = 255;                       /* position 9's block: page B 32 + 80 (tested) */
    st7565_seg_encode(ram, d);
    CHECK(ram[2][120 + 80 + 32] == 1 && ram[2][120 + 80 + 14] == 1, "meter position 9, box");
}

/* ---------- renderer ---------- */

static void line_text(int d, int l, char *out)
{
    memcpy(out, shown[d].line[l], 8);
    out[8] = 0;
}

static int line_is(int d, int l, const char *want)
{
    char t[9];
    line_text(d, l, t);
    return !strcmp(t, want);
}

static void test_ccpage(void)
{
    screen_t s = {0};
    s.kind = SCR_CCPAGE;
    s.u.ccpage.title = "Knobs  Page 2";
    s.u.ccpage.n = 8;
    static const char *cc[8] = {"14", "15", "16", "17", "18", "19", "20", "21"};
    for (int k = 0; k < 8; k++) {
        s.u.ccpage.knob[k].cc = cc[k];
        s.u.ccpage.knob[k].value = k * 18;
    }
    ui_renderer.show(&s);
    CHECK(line_is(0, 0, "Knobs   ") && line_is(0, 1, "Page 2  "), "title on display 1");
    CHECK(line_is(1, 0, "CC14    ") && line_is(1, 1, "       0"), "knob 1");
    CHECK(line_is(8, 0, "CC21    ") && line_is(8, 1, "     126"), "knob 8");
    CHECK(shown[1].meter == 0 && shown[8].meter == 253, "meters follow the values (%d %d)",
          shown[1].meter, shown[8].meter);
}

static void test_knobs(void)
{
    screen_t s = {0};
    s.kind = SCR_KNOBS;
    s.u.knobs.title = "Plug-in";
    s.u.knobs.right_a = "Synth";
    s.u.knobs.right_b = "2/6";
    static const char names[] = "CutoffResoTTTTTTTTTT";    /* slices, not terminated */
    for (int k = 0; k < 8; k++) {
        s.u.knobs.slot[k].name = k == 0 ? names : k == 1 ? names + 6 : "";
        s.u.knobs.slot[k].name_len = k == 0 ? 6 : k == 1 ? 4 : 0;
        s.u.knobs.slot[k].value = k == 0 ? "1.2kHz" : "";
    }
    s.u.knobs.slot[0].bar_style = BAR_FILL;
    s.u.knobs.slot[0].bar = 64;
    s.u.knobs.slot[0].bar_max = 127;
    ui_renderer.show(&s);
    CHECK(line_is(0, 0, "Plug-in ") && line_is(0, 1, "Synth   "), "title, device (2/6 does not fit)");
    CHECK(line_is(1, 0, "Cutoff  ") && line_is(2, 0, "Reso    "), "names honour name_len");
    CHECK(line_is(1, 1, "12kHz   ") && (shown[1].dots[1] & 1), "the dot after 1 is a dot");
    CHECK(shown[1].meter == 128, "bar 64/127 -> %d", shown[1].meter);
}

static void test_message_scroll(void)
{
    screen_t s = {0};
    int scroll = 50;
    s.kind = SCR_MESSAGE;
    s.u.message.text = "one two three four five six seven eight nine ten eleven twelve thirteen "
                       "fourteen fifteen sixteen seventeen eighteen nineteen twenty, and "
                       "then the text goes on for a while so that it needs more than two lines";
    s.u.message.scroll = &scroll;
    ui_renderer.show(&s);
    CHECK(scroll >= 0 && scroll < 50, "scroll clamped (%d)", scroll);
    int clamped = scroll;
    scroll = 0;
    ui_renderer.show(&s);
    CHECK(line_is(0, 0, "one two ") && line_is(1, 0, "three   "), "first words, one display each");
    CHECK(clamped >= 1, "more than 2 lines of text (%d)", clamped);
}

static void test_overlays(void)
{
    screen_t s = {0};
    s.kind = SCR_PLAY;
    s.u.play.status = "CH1";
    s.u.play.centre = "My Kontrol";
    s.u.play.hint = "Push: menu";
    s.lost = 1;
    s.popup.show = 1;
    s.popup.title = "Knob 1";
    s.popup.value = "64";
    ui_renderer.show(&s);
    CHECK(line_is(0, 0, "Knob 1  ") && line_is(0, 1, "64      "), "the popup is on top");
    s.popup.show = 0;
    ui_renderer.show(&s);
    CHECK(line_is(0, 0, "CONNECTI") || line_is(0, 0, "CONNECTI"), "lost banner over line 1");
    CHECK(line_is(0, 1, "My      ") && line_is(1, 1, "Kontrol "), "words stay on one display");
}

int main(void)
{
#define RUN(t) do { current = #t; t(); } while (0)
    RUN(test_font);
    RUN(test_map_unique);
    RUN(test_map_spots);
    RUN(test_ccpage);
    RUN(test_knobs);
    RUN(test_message_scroll);
    RUN(test_overlays);
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("test_seg: all passed\n");
    return 0;
}
