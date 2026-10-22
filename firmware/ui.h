#pragma once
/* Menu and screen system. Input is delivered as events so the same code runs
   on the device and in the host preview (test/ui_preview.c). */
#include <stdint.h>

enum ui_event_type {
    EV_ENC_TURN,     /* value = detents, signed */
    EV_ENC_PUSH,
    EV_ENC_LEFT, EV_ENC_RIGHT, EV_ENC_UP, EV_ENC_DOWN,
    EV_BACK,         /* dedicated back/exit, or encoder-left at top level */
    EV_KNOB_TURN,    /* index = knob 0..7, value = signed delta */
    EV_KNOB_TOUCH,   /* index = knob, value = 1 touched / 0 released */
    EV_SHOW_MENU,    /* open the settings menu */
};

void ui_init(void);
void ui_event(uint8_t type, int8_t index, int16_t value);
void ui_tick_ms(uint32_t ms);  /* timeouts for popups */
void ui_render(void);           /* draws into the framebuffer if something changed */
int ui_dirty(void);
int ui_menu_open(void);
enum { UI_MENU_SCALE, UI_MENU_ARP, UI_MENU_KEYS, UI_MENU_KNOBS, UI_MENU_SYSTEM };
void ui_open_menu(int which);   /* jump straight into a submenu */
void ui_notify(const char *title, const char *value);  /* transient overlay, e.g. "Octave" "+1" */
void ui_set_daw_text(const char *top, const char *bottom); /* Mackie Control LCD mirror */

/* callbacks supplied by the application */
void app_settings_changed(void);
