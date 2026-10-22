#include "knobs.h"
#include "hw.h"
#include "hal.h"

#define GPIOA_CRL  REG(0x40010800)
#define GPIOB_CRH  REG(0x40010C04)
#define GPIOB_BSRR REG(0x40010C10)
#define ADC1_SR    REG(0x40012400)
#define ADC1_CR1   REG(0x40012404)
#define ADC1_CR2   REG(0x40012408)
#define ADC1_SMPR2 REG(0x40012410)
#define ADC1_SQR1  REG(0x4001242C)
#define ADC1_SQR3  REG(0x40012434)
#define ADC1_DR    REG(0x4001244C)

#define STEP 38          /* stock firmware: both wipers must move > 38 counts */

/* mux state -> knob for the PA4/PA5 pair; PA6/PA7 is the same + 4 */
static const uint8_t mux_knob[4] = {0, 2, 1, 3};

static uint16_t prev[8][2];
uint16_t knobs_raw[8][2];          /* latest wiper readings, for diagnostics */
static uint8_t mux, settle;
static uint8_t have_prev[8];

uint16_t knobs_adc(int ch)
{
    ADC1_SQR3 = ch;
    ADC1_CR2 |= 1;                       /* ADON again = start conversion */
    for (int t = 0; !(ADC1_SR & 2); t++) /* EOC */
        if (t > 2000)
            return 0;
    return ADC1_DR & 0xFFF;
}

static void set_mux(int m)
{
    GPIOB_BSRR = ((m & 1) ? 1 << 8 : 1 << 24) | ((m & 2) ? 1 << 9 : 1 << 25);
}

void knobs_init(void)
{
    RCC_APB2ENR |= (1 << 2) | (1 << 3) | (1 << 9);     /* GPIOA, GPIOB, ADC1 */
    GPIOA_CRL &= ~0xFFFF0000u;                          /* PA4-7 analog */
    GPIOB_CRH = (GPIOB_CRH & ~0x000000FFu) | 0x00000022;/* PB8/PB9 outputs */
    ADC1_CR1 = 0;
    ADC1_SMPR2 = 0x3FFFFFFF;                            /* 239.5 cycles, as stock */
    ADC1_SQR1 = 0;                                      /* 1 conversion */
    ADC1_CR2 = 1;                                       /* ADON */
    hal_delay_ms(1);
    ADC1_CR2 |= 1 << 3;                                 /* reset calibration */
    while (ADC1_CR2 & (1 << 3))
        ;
    ADC1_CR2 |= 1 << 2;                                 /* calibrate */
    while (ADC1_CR2 & (1 << 2))
        ;
    set_mux(0);
}

static int abs16(int v) { return v < 0 ? -v : v; }

/* Direction from the two wipers, as the stock firmware decodes it. */
static int decode(int knob, uint16_t a, uint16_t b)
{
    int da = a - prev[knob][0], db = b - prev[knob][1];
    if (abs16(da) <= STEP || abs16(db) <= STEP)
        return 0;
    int steps = (abs16(da) < abs16(db) ? abs16(da) : abs16(db)) / STEP;
    if (steps > 63) steps = 63;
    int up;
    if (da > 0 && db > 0)       up = b < a;
    else if (da > 0 && db < 0)  up = (a > b ? a : b) < 0x900;
    else if (da < 0 && db < 0)  up = a < b;
    else                        up = (a < b ? a : b) > 0x700;
    prev[knob][0] = a;
    prev[knob][1] = b;
    /* jitter filter: a lone 1-step reversal is noise (an idle knob flipped
       between two values); real reversals move further or repeat */
    static int8_t last_dir[8];
    int dir = up ? -1 : 1;         /* the stock "up" flag means counter-clockwise (tested) */
    if (dir != last_dir[knob] && steps == 1) {
        last_dir[knob] = dir;
        return 0;
    }
    last_dir[knob] = dir;
    return dir * steps;
}

int knobs_poll(uint8_t *knob_out, int8_t *delta_out)
{
    static int8_t pending_d = 0;
    static uint8_t pending_k = 0xFF;
    if (pending_k != 0xFF) {             /* second knob of the previous read */
        *knob_out = pending_k;
        *delta_out = pending_d;
        pending_k = 0xFF;
        return 1;
    }
    /* one mux position per call: give the mux a call to settle, then read */
    if (settle) {
        settle = 0;
        return 0;
    }
    uint16_t v[4];
    for (int ch = 0; ch < 4; ch++) {
        uint32_t sum = 0;
        for (int i = 0; i < 4; i++)
            sum += knobs_adc(4 + ch);
        v[ch] = sum / 4;
    }
    int k0 = mux_knob[mux], k1 = k0 + 4;
    knobs_raw[k0][0] = v[0]; knobs_raw[k0][1] = v[1];
    knobs_raw[k1][0] = v[2]; knobs_raw[k1][1] = v[3];
    int d0 = 0, d1 = 0;
    if (have_prev[k0]) d0 = decode(k0, v[0], v[1]);
    else { prev[k0][0] = v[0]; prev[k0][1] = v[1]; have_prev[k0] = 1; }
    if (have_prev[k1]) d1 = decode(k1, v[2], v[3]);
    else { prev[k1][0] = v[2]; prev[k1][1] = v[3]; have_prev[k1] = 1; }

    mux = (mux + 1) & 3;
    set_mux(mux);
    settle = 1;

    /* report at most one knob per call; keep the second for the next call */
    if (d0 && d1) {
        pending_k = k1;
        pending_d = d1;
    }
    if (d0) { *knob_out = k0; *delta_out = d0; return 1; }
    if (d1) { *knob_out = k1; *delta_out = d1; pending_k = 0xFF; return 1; }
    return 0;
}
