#include "i2c_bb.h"
#include "hw.h"

#define CRL(b)  REG((b)->port + 0x00)
#define CRH(b)  REG((b)->port + 0x04)
#define IDR(b)  REG((b)->port + 0x08)
#define BSRR(b) REG((b)->port + 0x10)

static void wait(const i2c_bus_t *b)
{
    for (volatile int i = 0; i < b->delay; i++)
        ;
}

static void pin_od(const i2c_bus_t *b, int pin)
{
    volatile uint32_t *cr = pin < 8 ? &CRL(b) : &CRH(b);
    int sh = (pin & 7) * 4;
    *cr = (*cr & ~(0xFu << sh)) | (0x7u << sh);   /* open-drain output, 50 MHz */
}

static void high(const i2c_bus_t *b, int pin) { BSRR(b) = 1u << pin; }
static void low(const i2c_bus_t *b, int pin) { BSRR(b) = 1u << (pin + 16); }
static int read(const i2c_bus_t *b, int pin) { return (IDR(b) >> pin) & 1; }

/* release SCL and wait for the slave to let go of it (clock stretching) */
static int scl_high(const i2c_bus_t *b)
{
    high(b, b->scl);
    for (int t = 0; !read(b, b->scl); t++) {
        if (t > 20000)
            return -1;
    }
    wait(b);
    return 0;
}

void i2c_init(const i2c_bus_t *b)
{
    high(b, b->scl);
    high(b, b->sda);
    pin_od(b, b->scl);
    pin_od(b, b->sda);
    wait(b);
}

static int start(const i2c_bus_t *b)
{
    high(b, b->sda);
    if (scl_high(b))
        return -1;
    low(b, b->sda);
    wait(b);
    low(b, b->scl);
    wait(b);
    return 0;
}

static void stop(const i2c_bus_t *b)
{
    low(b, b->sda);
    wait(b);
    scl_high(b);
    high(b, b->sda);
    wait(b);
}

/* returns 0 on ACK */
static int put(const i2c_bus_t *b, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        if (v & (1 << i)) high(b, b->sda); else low(b, b->sda);
        wait(b);
        if (scl_high(b))
            return -1;
        low(b, b->scl);
    }
    high(b, b->sda);                     /* release for ACK */
    wait(b);
    if (scl_high(b))
        return -1;
    int nack = read(b, b->sda);
    low(b, b->scl);
    wait(b);
    return nack ? -1 : 0;
}

static int get(const i2c_bus_t *b, int ack)
{
    uint8_t v = 0;
    high(b, b->sda);
    for (int i = 7; i >= 0; i--) {
        wait(b);
        if (scl_high(b))
            return -1;
        v |= read(b, b->sda) << i;
        low(b, b->scl);
    }
    if (ack) low(b, b->sda); else high(b, b->sda);
    wait(b);
    if (scl_high(b))
        return -1;
    low(b, b->scl);
    high(b, b->sda);
    wait(b);
    return v;
}

int i2c_write(const i2c_bus_t *b, uint8_t addr, const uint8_t *d, int n)
{
    int r = start(b) || put(b, addr << 1);
    for (int i = 0; !r && i < n; i++)
        r = put(b, d[i]);
    stop(b);
    return r ? -1 : 0;
}

int i2c_write_reg(const i2c_bus_t *b, uint8_t addr, uint8_t reg, const uint8_t *d, int n)
{
    int r = start(b) || put(b, addr << 1) || put(b, reg);
    for (int i = 0; !r && i < n; i++)
        r = put(b, d[i]);
    stop(b);
    return r ? -1 : 0;
}

int i2c_read_reg(const i2c_bus_t *b, uint8_t addr, uint8_t reg, uint8_t *d, int n)
{
    int r = start(b) || put(b, addr << 1) || put(b, reg);
    if (!r) {
        stop(b);
        r = start(b) || put(b, (addr << 1) | 1);
    }
    for (int i = 0; !r && i < n; i++) {
        int v = get(b, i < n - 1);
        if (v < 0)
            r = -1;
        else
            d[i] = v;
    }
    stop(b);
    return r ? -1 : 0;
}
