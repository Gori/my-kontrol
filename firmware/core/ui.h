#pragma once
/* Screens and menus. Input arrives as UI_* events so the same code runs on
   the device and in the host preview (test/ui_preview.c). */
#include <stdint.h>

enum ui_input {
    UI_TURN,         /* value = detents, signed */
    UI_PUSH,
    UI_LEFT, UI_RIGHT, UI_UP, UI_DOWN,
    UI_BACK,
};

enum { UI_MENU_SCALE, UI_MENU_ARP, UI_MENU_KEYS, UI_MENU_CONTROLS, UI_MENU_PRESETS, UI_MENU_SYSTEM };

void ui_init(void);
void ui_splash(void);                       /* draws the boot screen into gfx_fb */
int ui_input(uint8_t in, int16_t value);    /* 1 if a menu or dialog took it */
int ui_menu_open(void);                     /* a menu or dialog owns the 4D encoder */
void ui_open_menu(int which);               /* straight into a submenu; Back returns home */
void ui_toggle_menu(void);
void ui_home_step(int d);                   /* next / previous home page (no DAW) */
void ui_tick_ms(uint32_t ms);
int ui_render(void);                        /* 1 if gfx_fb holds a new frame */
void ui_invalidate(void);

/* transient popups (1.2 s) */
void ui_notify(const char *title, const char *value);
void ui_focus_knob(int knob);               /* live value of a knob, whatever it controls */
void ui_learn_begin(void);                  /* "hold a chord" dialog */
