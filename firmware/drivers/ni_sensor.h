#pragma once
/* NI's I2C sensor MCUs on the Komplete Kontrol S MK1 (docs/hardware/s-mk1.md, Sensor MCUs):
   0x12 (knobs: 9 x u16) and 0x15 (touch: ADC, 48 touch deltas, 2 switch inputs). Each is
   polled with a write of `03 05` and a read of the reply [05, len, payload].

   A bit-banged I2C master over the board's pin operations, and the pure decoding (host
   tested): replies, knob turns from positions, a strip position from its touch nodes. */
#include <stdint.h>

typedef struct {
    void (*scl)(int release);         /* 0 = drive low, 1 = release (pulled up) */
    void (*sda)(int release);
    int (*scl_read)(void);
    int (*sda_read)(void);
    void (*half_bit)(void);           /* one half bit period (1.25 us for 400 kHz) */
} i2c_pins_t;

int i2c_write(const i2c_pins_t *p, uint8_t addr, const uint8_t *buf, int n);   /* 0 = acked */
int i2c_read(const i2c_pins_t *p, uint8_t addr, uint8_t *buf, int n);          /* 0 = acked */

#define NI_KNOBS_ADDR 0x12
#define NI_KNOBS_LEN  22
#define NI_TOUCH_ADDR 0x15
#define NI_TOUCH_LEN  102            /* 104 on the S88 */
#define NI_TOUCH_NODES 48

/* write 03 05, read a reply of len bytes into buf; 0 = a valid reply */
int ni_sensor_poll(const i2c_pins_t *p, uint8_t addr, uint8_t *buf, int len);

typedef struct {
    uint16_t value[9];               /* 8 knobs, then one more (aftertouch? not confirmed) */
} ni_knobs_t;

typedef struct {
    uint16_t adc;                    /* expression pedal (guess) */
    int16_t touch[NI_TOUCH_NODES];   /* level minus baseline per touch node */
    uint8_t switches;                /* bit 0, bit 1: the two switch inputs */
} ni_touch_t;

void ni_knobs_decode(const uint8_t *reply, ni_knobs_t *out);
void ni_touch_decode(const uint8_t *reply, ni_touch_t *out);

/* knob turn from two positions of an endless knob (16-bit wrap): steps of `unit`, the
   remainder kept in *acc */
int ni_knob_steps(uint16_t prev, uint16_t now, int unit, int16_t *acc);

/* a strip from its n touch nodes (in order along the strip): the centroid of the nodes above
   threshold as 0..16383, or -1 when none is touched */
int ni_strip_position(const int16_t *touch, const uint8_t *nodes, int n, int16_t threshold);
