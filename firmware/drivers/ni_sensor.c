/* NI sensor MCUs over bit-banged I2C (ni_sensor.h). Our code. */
#include "ni_sensor.h"

static void stretch(const i2c_pins_t *p)
{
    p->scl(1);
    for (int i = 0; i < 200 && !p->scl_read(); i++)    /* the slave may hold SCL low */
        p->half_bit();
}

static void start(const i2c_pins_t *p)
{
    p->sda(1);
    stretch(p);
    p->half_bit();
    p->sda(0);
    p->half_bit();
    p->scl(0);
}

static void stop(const i2c_pins_t *p)
{
    p->sda(0);
    p->half_bit();
    stretch(p);
    p->half_bit();
    p->sda(1);
    p->half_bit();
}

/* one byte out, MSB first; 0 if acked */
static int put(const i2c_pins_t *p, uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        p->sda(b >> i & 1);
        p->half_bit();
        stretch(p);
        p->half_bit();
        p->scl(0);
    }
    p->sda(1);
    p->half_bit();
    stretch(p);
    int nack = p->sda_read();
    p->half_bit();
    p->scl(0);
    return nack;
}

static uint8_t get(const i2c_pins_t *p, int ack)
{
    uint8_t b = 0;
    p->sda(1);
    for (int i = 0; i < 8; i++) {
        p->half_bit();
        stretch(p);
        b = (uint8_t)(b << 1 | (p->sda_read() & 1));
        p->half_bit();
        p->scl(0);
    }
    p->sda(!ack);
    p->half_bit();
    stretch(p);
    p->half_bit();
    p->scl(0);
    p->sda(1);
    return b;
}

int i2c_write(const i2c_pins_t *p, uint8_t addr, const uint8_t *buf, int n)
{
    start(p);
    int r = put(p, (uint8_t)(addr << 1));
    for (int i = 0; i < n && !r; i++)
        r = put(p, buf[i]);
    stop(p);
    return r;
}

int i2c_read(const i2c_pins_t *p, uint8_t addr, uint8_t *buf, int n)
{
    start(p);
    int r = put(p, (uint8_t)(addr << 1 | 1));
    for (int i = 0; i < n && !r; i++)
        buf[i] = get(p, i < n - 1);
    stop(p);
    return r;
}

int ni_sensor_poll(const i2c_pins_t *p, uint8_t addr, uint8_t *buf, int len)
{
    static const uint8_t poll[2] = {0x03, 0x05};
    if (i2c_write(p, addr, poll, 2) || i2c_read(p, addr, buf, len))
        return -1;
    return buf[0] == 0x05 && buf[1] == len ? 0 : -1;
}

static uint16_t u16(const uint8_t *b)
{
    return (uint16_t)(b[0] | b[1] << 8);
}

void ni_knobs_decode(const uint8_t *r, ni_knobs_t *out)
{
    for (int i = 0; i < 9; i++)
        out->value[i] = u16(r + 2 + 2 * i);
}

void ni_touch_decode(const uint8_t *r, ni_touch_t *out)
{
    out->adc = u16(r + 2);
    for (int i = 0; i < NI_TOUCH_NODES; i++)
        out->touch[i] = (int16_t)u16(r + 4 + 2 * i);
    out->switches = r[4 + 2 * NI_TOUCH_NODES];
}

int ni_knob_steps(uint16_t prev, uint16_t now, int unit, int16_t *acc)
{
    int a = *acc + (int16_t)(uint16_t)(now - prev);
    int steps = a / unit;
    *acc = (int16_t)(a - steps * unit);
    return steps;
}

int ni_strip_position(const int16_t *touch, const uint8_t *nodes, int n, int16_t threshold)
{
    int32_t sum = 0, weighted = 0;
    for (int i = 0; i < n; i++) {
        int v = touch[nodes[i]] - threshold;
        if (v > 0) {
            sum += v;
            weighted += (int32_t)v * i;
        }
    }
    if (!sum)
        return -1;
    return (int)(weighted * 16383 / ((int32_t)(n - 1) * sum));
}
