/* Host tests for the S-series panel drivers: drivers/shift595.c (chain bit order, planes),
   drivers/ni_keybed.c (messages, velocity), drivers/ni_sensor.c (replies, knob steps, strip
   position) and the light guide colours (core/lightguide.c). */
#include <stdio.h>
#include <string.h>
#include "lightguide.h"
#include "ni_keybed.h"
#include "ni_sensor.h"
#include "shift595.h"

static int failures;
static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, current); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- shift595 ---------- */

/* the chain a 74HC595 cascade holds after shifting bytes in order, LSB first: the last bit
   shifted sits at position 0 of the buffer numbering (byte i/8, bit i%8) */
static void chain_after(const uint8_t *bytes, int nbytes, uint8_t *bit_out)
{
    int total = nbytes * 8, k = 0;
    for (int j = 0; j < nbytes; j++)
        for (int b = 0; b < 8; b++, k++) {
            int idx = 8 * (nbytes - 1 - k / 8) + k % 8;      /* xmos.md §8.3 */
            bit_out[idx] = bytes[j] >> b & 1;
        }
    (void)total;
}

static void test_shift595(void)
{
    uint8_t level[80] = {0}, out[10], bits[80];
    level[0] = 127;                     /* on in every plane */
    level[9] = 1;                       /* plane 0 only */
    level[79] = 64;                     /* plane 6 only */
    for (int k = 0; k < SHIFT595_PLANES; k++) {
        shift595_plane(level, 80, k, out);
        chain_after(out, 10, bits);
        CHECK(bits[0] == 0, "plane %d: bit 0 on (active low)", k);
        CHECK(bits[9] == (k == 0 ? 0 : 1), "plane %d: bit 9", k);
        CHECK(bits[79] == (k == 6 ? 0 : 1), "plane %d: bit 79", k);
        int off = 0;
        for (int i = 0; i < 80; i++)
            off += bits[i];
        CHECK(off == 80 - 1 - (k == 0) - (k == 6), "plane %d: the rest off (%d)", k, off);
    }
    memset(level, 0, sizeof level);
    shift595_plane(level, 80, 3, out);
    CHECK(out[0] == 0xFF && out[9] == 0xFF, "all off: 0xFF");
    level[72] = 8;                      /* the first byte sent holds bits 72..79 */
    shift595_plane(level, 80, 3, out);
    CHECK(out[0] == 0xFE && out[1] == 0xFF, "bit 72 is the first byte, LSB (got %02x)", out[0]);
    /* the time a light is on over a period: the sum of its plane weights = its level */
    for (int v = 0; v < 128; v += 9) {
        level[5] = (uint8_t)v;
        int on = 0;
        for (int k = 0; k < SHIFT595_PLANES; k++) {
            shift595_plane(level, 80, k, out);
            chain_after(out, 10, bits);
            on += bits[5] ? 0 : 1 << k;
        }
        CHECK(on == v, "level %d is on for %d units", v, on);
    }
}

/* ---------- keybed ---------- */

static void test_keybed(void)
{
    ni_keybed_t k = {0};
    uint8_t note, on;
    uint16_t raw;
    const uint8_t msg[] = {0x94, 60, 88, 0x80, 60, 0, 0x3C, 0x91, 48, 1};
    int got = 0;
    for (unsigned i = 0; i < sizeof msg; i++)
        if (ni_keybed_byte(&k, msg[i], &note, &on, &raw)) {
            got++;
            if (got == 1)
                CHECK(note == 60 && on && raw == (4 << 7 | 88), "note on, raw %d", raw);
            if (got == 2)
                CHECK(note == 60 && !on && raw == 0, "note off");
            if (got == 3)
                CHECK(note == 48 && on && raw == 129, "after a stray data byte");
        }
    CHECK(got == 3, "%d messages", got);
    CHECK(ni_keybed_velocity(0) == 127 && ni_keybed_velocity(28) == 127, "fastest: 127");
    CHECK(ni_keybed_velocity(1023) == 1 && ni_keybed_velocity(650) == 1, "slowest: 1");
    int prev = 128;
    for (int r = 0; r < 1024; r++) {
        int v = ni_keybed_velocity((uint16_t)r);
        CHECK(v >= 1 && v <= 127 && v <= prev, "monotonic at %d", r);
        prev = v;
    }
    CHECK(ni_keybed_velocity(100) == 80, "raw 100 -> 80");
}

/* ---------- sensors ---------- */

static void test_sensors(void)
{
    uint8_t r[NI_TOUCH_LEN] = {5, NI_KNOBS_LEN};
    for (int i = 0; i < 9; i++)
        r[2 + 2 * i] = (uint8_t)(i * 3), r[3 + 2 * i] = (uint8_t)i;
    ni_knobs_t k;
    ni_knobs_decode(r, &k);
    CHECK(k.value[0] == 0 && k.value[8] == (8 << 8 | 24), "knob values little endian");

    memset(r, 0, sizeof r);
    r[0] = 5, r[1] = NI_TOUCH_LEN, r[2] = 0x34, r[3] = 0x12;
    r[4 + 2 * 3] = 0x10, r[5 + 2 * 3] = 0xFF;          /* node 3: -240 */
    r[4 + 2 * 47] = 0xE8, r[5 + 2 * 47] = 0x03;        /* node 47: 1000 */
    r[100] = 2;
    ni_touch_t t;
    ni_touch_decode(r, &t);
    CHECK(t.adc == 0x1234 && t.touch[3] == -240 && t.touch[47] == 1000 && t.switches == 2,
          "touch reply fields");

    int16_t acc = 0;
    CHECK(ni_knob_steps(1000, 1000 + 640, 64, &acc) == 10 && acc == 0, "640 units = 10 steps");
    CHECK(ni_knob_steps(10, 65530, 64, &acc) == 0 && acc == -16, "wraps: -16 units kept");
    CHECK(ni_knob_steps(0, 48, 64, &acc) == 0 && acc == 32, "remainder accumulates");
    CHECK(ni_knob_steps(0, 32, 64, &acc) == 1 && acc == 0, "and makes a step");

    int16_t touch[48] = {0};
    const uint8_t nodes[4] = {10, 11, 12, 13};
    CHECK(ni_strip_position(touch, nodes, 4, 100) == -1, "untouched: -1");
    touch[10] = 600;
    CHECK(ni_strip_position(touch, nodes, 4, 100) == 0, "first node: 0");
    touch[10] = 0, touch[13] = 600;
    CHECK(ni_strip_position(touch, nodes, 4, 100) == 16383, "last node: 16383");
    touch[12] = 600;
    int p = ni_strip_position(touch, nodes, 4, 100);
    CHECK(p > 12000 && p < 15000, "between the last two: %d", p);
}

/* ---------- light guide ---------- */

static void test_lightguide(void)
{
    uint8_t c[3];
    uint16_t major = 1 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11;
    lightguide_colour(60, 0, major, 1, 0, c);
    CHECK(c[2] == 255 && c[0] == 0, "C in C major: the root, blue");
    lightguide_colour(64, 0, major, 1, 0, c);
    CHECK(c[0] == 48 && c[1] == 48 && c[2] == 48, "E: a scale note, dim white");
    lightguide_colour(61, 0, major, 1, 0, c);
    CHECK(!c[0] && !c[1] && !c[2], "C#: not in the scale, off");
    lightguide_colour(61, 0, major, 1, 1, c);
    CHECK(c[0] == 255 && c[1] == 255 && c[2] == 255, "a held key: white");
    lightguide_colour(61, 0, major, 0, 0, c);
    CHECK(!c[0] && !c[1] && c[2] == 24, "no scale: faint blue");
    lightguide_colour(11, 2, 1 | 1 << 9, 1, 0, c);
    CHECK(c[0] == 48, "B in D: 9 semitones up, wraps below the root");
}

int main(void)
{
#define RUN(t) do { current = #t; t(); } while (0)
    RUN(test_shift595);
    RUN(test_keybed);
    RUN(test_sensors);
    RUN(test_lightguide);
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("test_panel: all passed\n");
    return 0;
}
