/* The main loop, shared by every board. */
#include "app.h"
#include "gfx.h"
#include "hal.h"
#include "ui.h"
#include "usb.h"

#define SPLASH_MS 1200
#define FRAME_MS 20                 /* at most 50 frames per second */
#define EVENTS_PER_PASS 16

/* pixel displays get the framebuffer here; a segment board's renderer
   (ui/seg9.c) sends its own frame from ui_render() */
static void blit_all(void)
{
#if !BOARD_SEG_DISPLAYS
    for (int p = 0; p < DISP_H / 8; p++)
        board_display_page(p, &gfx_fb[p * DISP_W]);
#endif
}

void mk_main(void)
{
    ui_splash();
    blit_all();
    board_display_power(1);
    board_init();

    hal_tick_start();
    usb_init();
    app_init();
    uint32_t splash_until = hal_ms() + SPLASH_MS;

    uint32_t last_frame = 0;
    int page = -1;                  /* next band to send, -1 = frame done */
    int stable = 0;

    for (;;) {
#ifdef HAL_HAS_IDLE
        hal_idle();
#endif
        hal_watchdog_feed();
        usb_task();

        mk_event_t ev;
        for (int i = 0; i < EVENTS_PER_PASS && board_poll(&ev); i++)
            app_event(&ev);
        app_poll();
        usb_task();

        uint32_t now = hal_ms();
        if (!stable && now > 10000) {
            stable = 1;
            hal_mark_stable();
        }
        if ((int32_t)(now - splash_until) < 0)
            continue;
#if BOARD_SEG_DISPLAYS
        (void)page;
        if (now - last_frame >= FRAME_MS && ui_render())
            last_frame = now;
#else
        /* one 8-pixel band per pass keeps every pass short */
        if (page >= 0) {
            board_display_page(page, &gfx_fb[page * DISP_W]);
            if (++page >= DISP_H / 8)
                page = -1;
        } else if (now - last_frame >= FRAME_MS && ui_render()) {
            last_frame = now;
            page = 0;
        }
#endif
    }
}
