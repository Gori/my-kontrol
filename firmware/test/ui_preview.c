/* Runs the real UI against a scripted event sequence and writes each frame as
   a PBM file: cc -I.. ui_preview.c ../ui.c ../gfx.c ../settings.c ../smartplay.c */
#include <stdio.h>
#include "display.h"
#include "settings.h"
#include "ui.h"

static int frame;
static const char *outdir;

void display_blit(const uint8_t *buf)
{
    char path[256];
    snprintf(path, sizeof path, "%s/frame_%02d.pbm", outdir, frame++);
    FILE *f = fopen(path, "w");
    fprintf(f, "P1\n128 32\n");
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 128; x++)
            fputc(buf[(y >> 3) * 128 + x] >> (y & 7) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

void app_settings_changed(void) {}

static void ev(int t, int i, int v)
{
    ui_event(t, i, v);
    ui_render();
}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : ".";
    settings_defaults(&settings);
    ui_init();
    ui_render();                               /* 00 home */
    ev(EV_SHOW_MENU, 0, 0);                    /* 01 main menu */
    ev(EV_ENC_TURN, 0, 1);                     /* 02 select Arpeggiator */
    ev(EV_ENC_PUSH, 0, 0);                     /* 03 arp menu */
    ev(EV_ENC_PUSH, 0, 0);                     /* 04 edit Arp */
    ev(EV_ENC_TURN, 0, 1);                     /* 05 Arp On */
    ev(EV_ENC_PUSH, 0, 0);                     /* 06 done */
    ev(EV_ENC_TURN, 0, 7);                     /* 07 scroll to Tempo */
    ev(EV_ENC_LEFT, 0, 0);                     /* 08 back to main */
    ev(EV_ENC_UP, 0, 0);                       /* 09 Scale & Chord */
    ev(EV_ENC_PUSH, 0, 0);                     /* 10 scale menu */
    ev(EV_ENC_PUSH, 0, 0);                     /* 11 edit mode */
    ev(EV_ENC_TURN, 0, 1);                     /* 12 Quantize */
    ev(EV_ENC_PUSH, 0, 0);
    ev(EV_ENC_TURN, 0, 2);
    ev(EV_ENC_PUSH, 0, 0);
    ev(EV_ENC_TURN, 0, 4);                     /* 15 Scale = Dorian */
    ev(EV_ENC_PUSH, 0, 0);                     /* 16 */
    ev(EV_BACK, 0, 0);
    ev(EV_BACK, 0, 0);                         /* 18 home with scale + arp */
    ui_notify("Octave", "+1");
    ui_render();                               /* 19 overlay */
    ui_tick_ms(2000);
    settings.daw_mode = 1;
    ui_set_daw_text("Audio 1  Bass  Keys", "  0.0  -6.2  -12 ");
    ui_render();                               /* 20 Mackie mirror */
    return 0;
}
