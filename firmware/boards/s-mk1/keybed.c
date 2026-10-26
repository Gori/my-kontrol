/* The S MK1 keybed scanner (tile 0): a UART into 1K, 31 250 Bd 8N1, on its own thread;
   4E bit 0 is driven high first (as NI's firmware does; the scanner's enable, guess).
   Messages (drivers/ni_keybed.c) become EV_KEY events: key = note - BOARD_BASE_NOTE,
   value = velocity, 0 = released. */
#include "panel.h"
#include "ni_keybed.h"
#include "xs1.h"

#define KB_RX     PORT_1('K')
#define KB_ENABLE PORT_4('E')
#define BIT_TICKS 3200

evq_t keybed_events;
uint16_t keybed_dropped;
static uint32_t stack[40];             /* 76 bytes used at most (emulator, xtest standalone) */

static void keybed_thread(uint32_t arg)
{
    (void)arg;
    ni_keybed_t k = {0};
    xs1_setc(KB_ENABLE, SETC_INUSE_ON);
    xs1_out(KB_ENABLE, 1);
    xs1_setc(KB_RX, SETC_INUSE_ON);
    for (;;) {
        xs1_setd(KB_RX, 0);
        xs1_setc(KB_RX, SETC_COND_EQ);              /* the start bit */
        (void)xs1_in(KB_RX);
        uint32_t t = xs1_getts(KB_RX) + BIT_TICKS / 2, b = 0;
        xs1_setc(KB_RX, SETC_COND_FULL);
        int stop = 0;
        for (int i = 0; i < 9; i++) {
            t += BIT_TICKS;
            xs1_setpt(KB_RX, t);
            uint32_t v = xs1_in(KB_RX) & 1;
            if (i < 8)
                b |= v << i;
            else
                stop = v;
        }
        uint8_t note, on;
        uint16_t raw;
        if (stop && ni_keybed_byte(&k, (uint8_t)b, &note, &on, &raw)) {
            int key = note - BOARD_BASE_NOTE;
            if (key < 0 || key >= BOARD_KEYS)
                continue;
            if (!evq_push(&keybed_events, EV_KEY, (uint8_t)key, on ? ni_keybed_velocity(raw) : 0))
                keybed_dropped++;
        }
    }
}

void keybed_start(void)
{
    xs1_thread_start(keybed_thread, 0, stack, sizeof stack / 4);
}
