/* Komplete Kontrol M32: the board side of core/hal.h. */
#include "hal.h"
#include "hw.h"
#include "keybed.h"
#include "knobs.h"
#include "leds.h"
#include "display.h"
#include "touch.h"
#include "strips.h"
#include "pedal.h"

const board_info_t board_info = {
    .name = "Komplete Kontrol M32",
    .short_name = "M32",
    .id = 1,
    .keys = BOARD_KEYS,
    .base_note = 41,            /* lowest key is F (F1 with C3 = 60) */
    .knobs = BOARD_KNOBS,
    .caps = CAP_STRIPS | CAP_KNOB_TOUCH | CAP_4D | CAP_PEDAL,
};

/* which sensors answered; a sensor that stops answering is retried slowly */
enum { HW_LEDS = 1, HW_TOUCH = 2, HW_PITCH = 4, HW_MOD = 8 };
static uint8_t hw_status;
static uint8_t fails[3];
static uint32_t retry_ms[3];

void board_early_init(void)
{
    keybed_pins();
    display_init();
}

int board_boot_combo(void)
{
    return keybed_boot_combo();
}

void board_init(void)
{
    knobs_init();
    if (leds_init() == 0) hw_status |= HW_LEDS;
    if (touch_init() == 0) hw_status |= HW_TOUCH;
    hw_status |= strips_init() << 2;
    pedal_init();
    keybed_init();
}

/* platform tick, every 100 us (interrupt) */
void board_tick(void)
{
    keybed_tick();
}

/* Strip position 1..200 to 0..16383. The outer 12 at each end are dead
   zones so both extremes are reachable (fingers land at about 12..188). */
#define STRIP_LO 12
#define STRIP_HI 188

static int16_t strip_value(uint8_t pos)
{
    if (pos < 3)
        return -1;
    int p = pos < STRIP_LO ? STRIP_LO : pos > STRIP_HI ? STRIP_HI : pos;
    return (p - STRIP_LO) * 16383 / (STRIP_HI - STRIP_LO);
}

/* I2C sensors: after 20 failed reads in a row stop polling and retry once
   a second, so a dead chip costs nothing in the main loop. */
static int sensor_ok(int i, uint8_t bit, uint32_t now)
{
    if (hw_status & bit)
        return 1;
    if (now - retry_ms[i] < 1000)
        return 0;
    retry_ms[i] = now;
    return 1;
}

static void sensor_result(int i, uint8_t bit, int ok)
{
    if (ok) {
        fails[i] = 0;
        hw_status |= bit;
    } else if (++fails[i] >= 20) {
        fails[i] = 20;
        hw_status &= ~bit;
    }
}

int board_poll(mk_event_t *ev)
{
    static uint32_t last_ms, touch_acc, strip_acc;
    static uint8_t touch_prev, which;
    static int16_t strip_prev[2] = {-1, -1};

    if (keybed_next(ev))
        return 1;

    static uint8_t touch_cur;
    if (touch_cur != touch_prev) {          /* one knob per call */
        uint8_t changed = touch_cur ^ touch_prev;
        for (int i = 0; i < BOARD_KNOBS; i++)
            if (changed >> i & 1) {
                touch_prev ^= 1 << i;
                *ev = (mk_event_t){EV_KNOB_TOUCH, i, touch_cur >> i & 1};
                return 1;
            }
    }

    uint8_t k;
    int8_t d;
    if (knobs_poll(&k, &d)) {
        *ev = (mk_event_t){EV_KNOB, k, d};
        return 1;
    }

    uint32_t now = hal_ms(), dt = now - last_ms;
    last_ms = now;
    touch_acc += dt;
    strip_acc += dt;

    if (strip_acc >= 5) {                   /* each strip every 10 ms */
        strip_acc = 0;
        which ^= 1;
        uint8_t bit = which ? HW_MOD : HW_PITCH;
        uint8_t pos;
        if (sensor_ok(1 + which, bit, now)) {
            int ok = strips_read(which, &pos) == 0;
            sensor_result(1 + which, bit, ok);
            if (ok) {
                int16_t v = strip_value(pos);
                if (v != strip_prev[which]) {
                    strip_prev[which] = v;
                    *ev = (mk_event_t){EV_STRIP, which, v};
                    return 1;
                }
            }
        }
    }

    if (touch_acc >= 10) {
        touch_acc = 0;
        uint8_t m;
        if (sensor_ok(0, HW_TOUCH, now)) {
            int r = touch_poll(&m);
            sensor_result(0, HW_TOUCH, r >= 0);
            if (r > 0)
                touch_cur = m;
        }
        uint8_t sw, down;
        if (pedal_poll(&sw, &down)) {
            *ev = (mk_event_t){EV_PEDAL, sw, down};
            return 1;
        }
    }
    return 0;
}

void board_light(uint8_t ctl, uint8_t level)
{
    if (ctl < LED_COUNT)             /* LED index = control ID on the M32 */
        leds_set(ctl, level);
}

void board_lights_flush(void)
{
    leds_flush();
}

void board_display_page(int page, const uint8_t *cols) { display_page(page, cols); }
void board_display_contrast(uint8_t v) { display_contrast(v); }
void board_display_power(int on) { display_power(on); }

/* The F1 has no internal D+ pull-up; the board switches one with PA8. */
void board_usb_pullup(int on)
{
    static int init;
    if (!init) {
        init = 1;
        RCC_APB2ENR |= 1 << 2;
        GPIOA_CRH = (GPIOA_CRH & ~0xFu) | 0x2;      /* PA8 push-pull output */
    }
    if (on)
        GPIOA_BSRR = 1 << 8;
    else
        GPIOA_BRR = 1 << 8;
}

/* hw_status, strip probe bytes, raw knob wipers and pedal readings */
int board_diag(uint8_t *buf, int max)
{
    int n = 0;
    if (n < max) buf[n++] = hw_status;
    for (int i = 0; i < 6 && n < max; i++)
        buf[n++] = strips_probe_raw[i / 3][i % 3];
    for (int i = 0; i < 8 && n + 4 <= max; i++) {
        buf[n++] = knobs_raw[i][0];
        buf[n++] = knobs_raw[i][0] >> 8;
        buf[n++] = knobs_raw[i][1];
        buf[n++] = knobs_raw[i][1] >> 8;
    }
    for (int i = 0; i < 3 && n + 2 <= max; i++) {
        buf[n++] = pedal_raw[i];
        buf[n++] = pedal_raw[i] >> 8;
    }
    if (n + 2 <= max) {
        buf[n++] = keybed_dropped;
        buf[n++] = keybed_dropped >> 8;
    }
    return n;
}

/* NI's firmware keeps its USB serial string at 0x0801FBA7: a 0x03 byte,
   then 8 UTF-16LE characters (e.g. "00002701"); the bootloader reports the
   same one. Reusing it keeps the keyboard recognisable across firmwares. */
int board_serial(char *out, int max)
{
    const volatile uint8_t *d = (const volatile uint8_t *)0x0801FBA7;
    if (d[0] != 3)
        return 0;
    int n = 0;
    for (int i = 1; i < 17 && n < max - 1; i += 2) {
        uint8_t c = d[i];
        if (d[i + 1] || c < 0x21 || c > 0x7E)
            break;
        out[n++] = c;
    }
    out[n] = 0;
    return n >= 4 ? n : 0;
}
