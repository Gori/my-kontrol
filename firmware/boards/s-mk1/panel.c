/* S MK1 panel inputs (tile 0): the 4 input shift registers every 1 ms (from the 100 us
   tick), NI's two sensor MCUs every 10 ms (their own thread: an I2C poll takes about 3 ms,
   which would stall the encoder). Hardware: docs/hardware/s-mk1.md.

   Input registers (bus C): clock 1B (clock block 3, ref/86 = 1.163 MHz), load 1C (active
   low), data 1A (8 bits per input, LSB first). Inputs are active low, debounced over 3 scans.
   What each of the 32 bits is, as far as known (bit = byte * 8 + bit):
     5, 6      pedal switches tip / ring (EV_PEDAL): NI's firmware sends CC64 / CC65 for them
               (emulator; the hardware doc also lists sensor 0x15's switch inputs as a
               candidate: TODO confirm on a keyboard)
     7         part of the model strap (bits 7..5 at power-up, NI); ignored
     8, 9      the Control encoder's quadrature (EV_ENCODER)
     28, 29    the page buttons (NI switches the knob page); which is left: TODO. Both held
               at power-up = the update-mode combo (panel_boot_combo)
     the rest  TODO: unknown; their state is in board_diag for mapping on a keyboard. Still
               to find among them: the transport (CTL_LOOP, CTL_RWD, CTL_FFW, CTL_PLAY,
               CTL_REC, CTL_STOP), the encoder push (CTL_NAV_PUSH), CTL_BACK, CTL_ENTER,
               Shift, Scale, Arp, the octave and preset buttons, Browse, Instance

   Sensor MCUs (I2C SCL 1H, SDA 1F, open drain, 400 kHz): 0x12 knob positions -> EV_KNOB,
   0x15 touch nodes -> EV_KNOB_TOUCH, CTL_ENCODER_TOUCH, EV_STRIP, and its ADC -> EV_EXPRESSION.
   Which touch node is which control, the knob resolution and the touch threshold are not
   known yet: the tables below are placeholders (TODO), easy to change once measured.
   NI's start-up configuration writes (0x60 ...) are not sent: their content is unknown.
   The 9th value of 0x12 (aftertouch?) and 0x15's switch bits are not used (TODO). */
#include "panel.h"
#include "ni_sensor.h"
#include "xs1.h"

evq_t panel_events, sensor_events;
uint16_t panel_dropped;

int evq_push(evq_t *q, uint8_t type, uint8_t id, int16_t value)
{
    uint8_t h = q->head, n = (uint8_t)((h + 1) & 31);
    if (n == q->tail)
        return 0;
    q->buf[h] = (uint32_t)type << 24 | (uint32_t)id << 16 | (uint16_t)value;
    q->head = n;
    return 1;
}

int evq_pop(evq_t *q, mk_event_t *ev)
{
    uint8_t t = q->tail;
    if (t == q->head)
        return 0;
    uint32_t e = q->buf[t];
    q->tail = (uint8_t)((t + 1) & 31);
    ev->type = (uint8_t)(e >> 24);
    ev->id = (uint8_t)(e >> 16);
    ev->value = (int16_t)(uint16_t)e;
    return 1;
}

static void emit_to(evq_t *q, uint8_t type, uint8_t id, int16_t value)
{
    if (!evq_push(q, type, id, value))
        panel_dropped++;
}

static void emit(uint8_t type, uint8_t id, int16_t value)
{
    emit_to(&panel_events, type, id, value);
}

static void sensor_emit(uint8_t type, uint8_t id, int16_t value)
{
    emit_to(&sensor_events, type, id, value);
}

/* ---------- input registers ---------- */

#define IN_CLK  PORT_1('B')
#define IN_LOAD PORT_1('C')
#define IN_DATA PORT_1('A')
#define IN_CB   CLKBLK(3)

enum { BIT_TIP = 5, BIT_RING = 6, BIT_ENC_A = 8, BIT_ENC_B = 9 };

static const uint8_t bit_ctl[32] = {   /* CTL_* + 1, 0 = none or not known */
    [28] = CTL_PAGE_LEFT + 1,       /* TODO: left or right */
    [29] = CTL_PAGE_RIGHT + 1,
};

static uint32_t hist[3], state, raw_bits;
static int scans, enc_state, enc_acc;

static uint32_t read_registers(void)
{
    xs1_out(IN_LOAD, 1);
    uint32_t t = xs1_getts(IN_LOAD), v = 0;
    xs1_setpt(IN_LOAD, t + 5);          /* a one-clock load pulse */
    xs1_out(IN_LOAD, 0);
    xs1_setpt(IN_LOAD, t + 6);
    xs1_out(IN_LOAD, 1);
    for (int i = 0; i < 4; i++) {       /* then 8 bits each, back to back */
        xs1_setpt(IN_DATA, t + 13 + 8 * i);
        v |= (xs1_in(IN_DATA) & 0xFF) << (8 * i);
    }
    return ~v;                          /* active low: 1 = pressed */
}

/* quadrature: +1/-1 per state change, one detent per 4 (TODO: check on a keyboard) */
static void encoder(uint32_t bits)
{
    static const int8_t step[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};
    int s = (int)(bits >> BIT_ENC_A & 3);
    enc_acc += step[enc_state << 2 | s];
    enc_state = s;
    if (enc_acc >= 4 || enc_acc <= -4) {
        emit(EV_ENCODER, 0, enc_acc > 0 ? 1 : -1);
        enc_acc = 0;
    }
}

static void scan(void)
{
    uint32_t v = read_registers();
    raw_bits = v;
    hist[2] = hist[1], hist[1] = hist[0], hist[0] = v;
    encoder(v);                         /* the encoder is not debounced */
    if (scans < 3) {                    /* start-up: take the state, report nothing */
        state = v;
        scans++;
        return;
    }
    uint32_t stable1 = hist[0] & hist[1] & hist[2], stable0 = ~(hist[0] | hist[1] | hist[2]);
    uint32_t next = (state | stable1) & ~stable0, changed = next ^ state;
    state = next;
    changed &= ~(3u << BIT_ENC_A);
    for (int b = 0; changed; b++, changed >>= 1) {
        if (!(changed & 1))
            continue;
        int down = state >> b & 1;
        if (b == BIT_TIP || b == BIT_RING)
            emit(EV_PEDAL, b == BIT_TIP ? 0 : 1, down);
        else if (bit_ctl[b])
            emit(EV_BUTTON, (uint8_t)(bit_ctl[b] - 1), down);
    }
}

/* ---------- sensor MCUs ---------- */

#define I2C_SCL PORT_1('H')
#define I2C_SDA PORT_1('F')

static uint32_t i2c_tmr, i2c_t;

static void pin(uint32_t p, int release)
{
    if (release)
        (void)xs1_in(p);                /* an input: the pull-up holds the line high */
    else
        xs1_out(p, 0);
}

static void scl(int r) { pin(I2C_SCL, r); }
static void sda(int r) { pin(I2C_SDA, r); }
static int scl_read(void) { return xs1_in(I2C_SCL) & 1; }
static int sda_read(void) { return xs1_in(I2C_SDA) & 1; }

static void half_bit(void)
{
    i2c_t += 125;                       /* 1.25 us */
    uint32_t now = xs1_time(i2c_tmr);
    if ((int32_t)(i2c_t - now) < 0)
        i2c_t = now;
    xs1_wait_until(i2c_tmr, i2c_t);
}

static const i2c_pins_t i2c = { scl, sda, scl_read, sda_read, half_bit };

/* placeholders until measured on a keyboard (TODO) */
#define KNOB_UNIT        64             /* sensor units per knob step */
#define TOUCH_ON         300            /* touch delta: touched above, released below OFF */
#define TOUCH_OFF        150
#define STRIP_THRESHOLD  100
static const uint8_t knob_node[8] = {0, 1, 2, 3, 4, 5, 6, 7};
#define ENCODER_NODE     8
static const uint8_t strip_node[2][8] = {
    {16, 17, 18, 19, 20, 21, 22, 23},   /* pitch */
    {24, 25, 26, 27, 28, 29, 30, 31},   /* mod */
};

static struct {
    uint8_t ok[2], fails[2];
    uint16_t retry[2];
    uint16_t knob[9];
    int16_t acc[8];
    uint16_t touched;                   /* bits 0..7 knobs, 8 encoder */
    int16_t strip[2];
    uint16_t expression;
    uint8_t have_knobs, have_touch;
} sn = { .strip = {-1, -1}, .expression = 0xFFFF };

static int sensor_due(int i)
{
    if (sn.fails[i] < 20)
        return 1;
    if (++sn.retry[i] < 100)            /* a dead sensor: once a second */
        return 0;
    sn.retry[i] = 0;
    return 1;
}

static void sensor_result(int i, int ok)
{
    sn.ok[i] = ok;
    sn.fails[i] = ok ? 0 : sn.fails[i] < 20 ? sn.fails[i] + 1 : 20;
}

static void touch_bit(int bit, int16_t delta, uint8_t type, uint8_t id)
{
    int was = sn.touched >> bit & 1;
    int now = was ? delta > TOUCH_OFF : delta > TOUCH_ON;
    if (now != was) {
        sn.touched ^= (uint16_t)(1u << bit);
        sensor_emit(type, id, now);
    }
}

static void sensors(void)
{
    uint8_t r[NI_TOUCH_LEN];
    i2c_t = xs1_time(i2c_tmr);
    if (sensor_due(0)) {
        int ok = ni_sensor_poll(&i2c, NI_KNOBS_ADDR, r, NI_KNOBS_LEN) == 0;
        sensor_result(0, ok);
        if (ok) {
            ni_knobs_t k;
            ni_knobs_decode(r, &k);
            for (int i = 0; i < 8 && sn.have_knobs; i++) {
                int steps = ni_knob_steps(sn.knob[i], k.value[i], KNOB_UNIT, &sn.acc[i]);
                if (steps)
                    sensor_emit(EV_KNOB, (uint8_t)i, (int16_t)(steps > 64 ? 64 : steps < -64 ? -64 : steps));
            }
            for (int i = 0; i < 9; i++)
                sn.knob[i] = k.value[i];
            sn.have_knobs = 1;
        }
    }
    if (sensor_due(1)) {
        int ok = ni_sensor_poll(&i2c, NI_TOUCH_ADDR, r, NI_TOUCH_LEN) == 0;
        sensor_result(1, ok);
        if (ok) {
            ni_touch_t t;
            ni_touch_decode(r, &t);
            for (int i = 0; i < 8; i++)
                touch_bit(i, t.touch[knob_node[i]], EV_KNOB_TOUCH, (uint8_t)i);
            touch_bit(8, t.touch[ENCODER_NODE], EV_BUTTON, CTL_ENCODER_TOUCH);
            for (int s = 0; s < 2; s++) {
                int p = ni_strip_position(t.touch, strip_node[s], 8, STRIP_THRESHOLD);
                int d = p - sn.strip[s];
                if ((p < 0) != (sn.strip[s] < 0) || d > 32 || d < -32) {
                    sn.strip[s] = (int16_t)p;
                    sensor_emit(EV_STRIP, s ? STRIP_MOD : STRIP_PITCH, (int16_t)p);
                }
            }
            uint16_t e = t.adc > 4095 ? 16383 : (uint16_t)(t.adc << 2);   /* 12-bit ADC */
            int de = e - sn.expression;
            if (sn.have_touch && (de > 64 || de < -64)) {
                sn.expression = e;
                sensor_emit(EV_EXPRESSION, 0, (int16_t)e);
            } else if (!sn.have_touch) {
                sn.expression = e;
            }
            sn.have_touch = 1;
        }
    }
}

/* ---------- */

static void inputs_init(void)
{
    static uint8_t done;
    if (done)
        return;
    done = 1;
    xs1_clock_init(IN_CB, 43);          /* 100 MHz / 86 */
    xs1_setc(IN_CLK, SETC_INUSE_ON);
    xs1_setc(IN_LOAD, SETC_INUSE_ON);
    xs1_setc(IN_DATA, SETC_INUSE_ON);
    xs1_setc(IN_DATA, SETC_BUFFERS);
    xs1_settw(IN_DATA, 8);
    xs1_setclk(IN_DATA, IN_CB);
    xs1_setclk(IN_LOAD, IN_CB);
    xs1_setc(IN_CLK, SETC_CLOCKPORT);
    xs1_setclk(IN_CLK, IN_CB);
    xs1_setc(IN_CB, SETC_START);
}

/* Power-up combo: NI's firmware takes another start-up path when input byte 3 reads 0xCF,
   bits 28 and 29 pressed and the rest of that byte released: both page buttons. Ours stops
   in update mode then. Three reads 1 ms apart must agree. */
int panel_boot_combo(void)
{
    inputs_init();
    for (int i = 0; i < 3; i++) {
        if ((read_registers() >> 24 & 0xFF) != 0x30)
            return 0;
        hal_delay_ms(1);
    }
    return 1;
}

void panel_init(void)
{
    inputs_init();
    xs1_setc(I2C_SCL, SETC_INUSE_ON);
    xs1_setc(I2C_SDA, SETC_INUSE_ON);
    (void)xs1_in(I2C_SCL);
    (void)xs1_in(I2C_SDA);
    i2c_tmr = xs1_getr(RES_TIMER);
}

void panel_tick(void)
{
    static uint8_t t1;
    if (++t1 < 10)
        return;
    t1 = 0;
    scan();
}

static uint32_t sensor_stack[112];     /* 360 bytes used at most (emulator) */

static void sensor_thread(uint32_t arg)
{
    (void)arg;
    uint32_t tmr = xs1_getr(RES_TIMER), next = xs1_time(tmr);
    for (;;) {
        next += 10 * TICKS_PER_MS;
        xs1_wait_until(tmr, next);
        sensors();                      /* about 3 ms of I2C */
    }
}

void sensors_start(void)
{
    xs1_thread_start(sensor_thread, 0, sensor_stack, sizeof sensor_stack / 4);
}

/* the raw input bits and which sensors answer */
int panel_diag(uint8_t *buf, int max)
{
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++)
        buf[n++] = (uint8_t)(raw_bits >> (8 * i));
    if (n < max)
        buf[n++] = (uint8_t)(sn.ok[0] | sn.ok[1] << 1);
    return n;
}
