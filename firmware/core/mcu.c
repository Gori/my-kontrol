#include "mcu.h"

char mcu_lcd[2][57];
uint8_t mcu_led[128];
uint8_t mcu_ring[8];
char mcu_timecode[11];
char mcu_assignment[3];
uint8_t mcu_connected;
uint16_t mcu_lcd_gen, mcu_timecode_gen;

static mcu_send_fn send;

/* Serial number reported in the handshake; any 7 ASCII bytes work. */
static const uint8_t serial[7] = {'M', 'K', 'O', 'N', 'T', 'R', 'L'};

/* Sysex header: F0 00 00 66 <model>; 0x14 = Mackie Control, 0x10 = Logic
   Control. Replies use the model the host addressed. */
static uint8_t model = 0x14;

static void send3(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m[3] = {a, b, c};
    send(m, 3);
}

void mcu_reset(void)
{
    mcu_connected = 0;
    for (int i = 0; i < 128; i++)
        mcu_led[i] = 0;
    for (int i = 0; i < 8; i++)
        mcu_ring[i] = 0;
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 56; i++)
            mcu_lcd[r][i] = ' ';
        mcu_lcd[r][56] = 0;
    }
    for (int i = 0; i < 10; i++)
        mcu_timecode[i] = ' ';
    mcu_timecode[10] = 0;
    mcu_assignment[0] = mcu_assignment[1] = ' ';
    mcu_assignment[2] = 0;
    mcu_lcd_gen++;
    mcu_timecode_gen++;
}

void mcu_init(mcu_send_fn s)
{
    send = s;
    mcu_reset();
}

void mcu_button(uint8_t note, int pressed)
{
    send3(0x90, note & 0x7F, pressed ? 127 : 0);
}

/* relative CC: 0x01..0x3F clockwise, 0x41..0x7F counter-clockwise */
static uint8_t rel(int d)
{
    if (d > 63) d = 63;
    if (d < -63) d = -63;
    return d >= 0 ? d : 0x40 | -d;
}

void mcu_vpot(uint8_t strip, int delta)
{
    if (delta)
        send3(0xB0, 0x10 + (strip & 7), rel(delta));
}

void mcu_jog(int delta)
{
    if (delta)
        send3(0xB0, 0x3C, rel(delta));
}

void mcu_fader(uint8_t strip, uint16_t v)
{
    send3(0xE0 | (strip & 7), v & 0x7F, (v >> 7) & 0x7F);
}

static void reply(uint8_t cmd, const uint8_t *data, uint8_t n)
{
    uint8_t m[24];
    uint8_t k = 0;
    m[k++] = 0xF0;
    m[k++] = 0x00;
    m[k++] = 0x00;
    m[k++] = 0x66;
    m[k++] = model;
    m[k++] = cmd;
    for (int i = 0; i < n && k < sizeof m - 1; i++)
        m[k++] = data[i];
    m[k++] = 0xF7;
    send(m, k);
}

void mcu_sysex(const uint8_t *m, uint16_t n)
{
    if (n < 7 || m[1] != 0 || m[2] != 0 || m[3] != 0x66 || (m[4] != 0x14 && m[4] != 0x10))
        return;
    model = m[4];
    mcu_connected = 1;
    switch (m[5]) {
    case 0x00: {    /* device query -> host connection query: serial + challenge */
        uint8_t d[11];
        for (int i = 0; i < 7; i++) d[i] = serial[i];
        d[7] = 'M'; d[8] = '3'; d[9] = '2'; d[10] = '!';
        reply(0x01, d, 11);
        break;
    }
    case 0x02:      /* host connection reply -> confirmation */
        reply(0x03, serial, 7);
        break;
    case 0x13: {    /* version request */
        static const uint8_t v[] = {'V', '1', '.', '0', '2'};
        reply(0x14, v, sizeof v);
        break;
    }
    case 0x1A:      /* serial request */
        reply(0x1B, serial, 7);
        break;
    case 0x12: {    /* LCD: offset, then ASCII */
        uint8_t off = m[6];
        for (uint16_t i = 7; i + 1 < n && off < 112; i++, off++) {
            char c = m[i] >= 0x20 && m[i] < 0x7F ? m[i] : ' ';
            mcu_lcd[off / 56][off % 56] = c;
        }
        mcu_lcd_gen++;
        break;
    }
    case 0x0F:      /* go offline */
        if (n > 6 && m[6] == 0x7F)
            mcu_reset();
        break;
    case 0x62:      /* all LEDs off */
        for (int i = 0; i < 128; i++)
            mcu_led[i] = 0;
        break;
    case 0x63:      /* reset */
        mcu_reset();
        break;
    default:
        break;
    }
}

void mcu_note(uint8_t note, uint8_t vel)
{
    mcu_connected = 1;
    mcu_led[note & 0x7F] = vel == 0 ? 0 : vel == 1 ? 1 : 2;
}

void mcu_cc(uint8_t cc, uint8_t v)
{
    mcu_connected = 1;
    if (cc >= 0x30 && cc <= 0x37) {
        mcu_ring[cc - 0x30] = v & 0x7F;      /* bits 4-5 mode, bit 6 centre LED, 0-3 position */
    } else if (cc >= 0x40 && cc <= 0x4B) {
        /* 0x40-0x49 timecode, 0x40 = rightmost digit; 0x4A/0x4B assignment
           display (right, left). Bit 6 of the value = dot. */
        uint8_t c = v & 0x3F;
        c = c < 0x20 ? c + 0x40 : c;
        if (cc >= 0x4A)
            mcu_assignment[0x4B - cc] = c;
        else
            mcu_timecode[9 - (cc - 0x40)] = c;
        mcu_timecode_gen++;
    }
}
