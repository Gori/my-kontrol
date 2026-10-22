/* Host tests for smartplay.c: cc -I.. test_smartplay.c ../smartplay.c && ./a.out */
#include <stdio.h>
#include <string.h>
#include "smartplay.h"

static int ev_note[256], ev_vel[256], ev_on[256], nev;
static int failures;

static void out(uint8_t note, uint8_t vel, int on)
{
    ev_note[nev] = note;
    ev_vel[nev] = vel;
    ev_on[nev] = on;
    nev++;
}

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static sp_config_t cfg;

static void reset(void)
{
    sp_defaults(&cfg);
    sp_init(&cfg, out);
    nev = 0;
}

/* count currently sounding notes from the event log */
static int sounding_count(void)
{
    int on[128] = {0}, n = 0;
    for (int i = 0; i < nev; i++)
        on[ev_note[i]] += ev_on[i] ? 1 : -1;
    for (int i = 0; i < 128; i++) {
        CHECK(on[i] >= 0, "note %d released more often than played", i);
        n += on[i] > 0;
    }
    return n;
}

static void test_passthrough(void)
{
    reset();
    sp_key(60, 100, 1);
    CHECK(nev == 1 && ev_note[0] == 60 && ev_vel[0] == 100 && ev_on[0], "passthrough on");
    sp_key(60, 0, 0);
    CHECK(nev == 2 && ev_note[1] == 60 && !ev_on[1], "passthrough off");
}

static void test_quantize(void)
{
    reset();
    cfg.scale_mode = SCALE_QUANTIZE;   /* C major */
    sp_key(61, 100, 1);                /* C# -> C (tie goes down) */
    CHECK(ev_note[0] == 60, "C# quantized to %d", ev_note[0]);
    sp_key(61, 0, 0);
    CHECK(ev_note[1] == 60 && !ev_on[1], "release of quantized note");
    cfg.root = 2;                      /* D major: F# in, F out */
    nev = 0;
    sp_key(65, 100, 1);
    CHECK(ev_note[0] == 64, "F in D major -> %d", ev_note[0]);
}

static void test_easy(void)
{
    reset();
    cfg.scale_mode = SCALE_EASY;
    cfg.scale = 1;                     /* C minor */
    sp_key(64, 100, 1);                /* E (3rd white key) -> Eb */
    CHECK(ev_note[0] == 63, "easy E in C minor -> %d", ev_note[0]);
    nev = 0;
    sp_key(61, 100, 1);                /* black key ignored */
    CHECK(nev == 0, "black key in easy mode produced %d events", nev);
    reset();
    cfg.scale_mode = SCALE_EASY;
    cfg.scale = 9;                     /* major pentatonic, 5 notes */
    sp_key(60, 100, 1);
    sp_key(71, 100, 1);                /* B = 7th white key -> wraps to D one octave up */
    CHECK(ev_note[0] == 60 && ev_note[1] == 74, "pentatonic easy: %d %d", ev_note[0], ev_note[1]);
}

static void test_chords(void)
{
    reset();
    cfg.chord = CHORD_TRIAD;           /* no scale: major triad */
    sp_key(60, 90, 1);
    CHECK(nev == 3 && ev_note[0] == 60 && ev_note[1] == 64 && ev_note[2] == 67, "C triad");
    sp_key(60, 0, 0);
    CHECK(sounding_count() == 0, "triad fully released");

    reset();
    cfg.scale_mode = SCALE_QUANTIZE;
    cfg.chord = CHORD_TRIAD;           /* diatonic: D in C major -> D F A */
    sp_key(62, 90, 1);
    CHECK(ev_note[0] == 62 && ev_note[1] == 65 && ev_note[2] == 69, "diatonic Dm: %d %d %d", ev_note[0], ev_note[1], ev_note[2]);

    /* changing chord type while held still releases what was played */
    cfg.chord = CHORD_SEVENTH;
    sp_config_changed();
    sp_key(62, 0, 0);
    CHECK(sounding_count() == 0, "config change mid-note leaves %d hanging", sounding_count());
}

static void run_us(uint32_t us)
{
    for (uint32_t t = 0; t < us; t += 100)
        sp_tick_us(100);
}

static void test_arp_up(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.arp_rate = RATE_1_16;
    cfg.bpm_x10 = 1200;                /* 16th = 125 ms */
    sp_init(&cfg, out);
    sp_key(60, 100, 1);
    sp_key(64, 100, 1);
    sp_key(67, 100, 1);
    run_us(125000 * 4 + 1000);
    int ons[16], n = 0;
    for (int i = 0; i < nev; i++)
        if (ev_on[i] && n < 16)
            ons[n++] = ev_note[i];
    CHECK(n >= 5, "arp steps: %d", n);
    CHECK(ons[0] == 60 && ons[1] == 64 && ons[2] == 67 && ons[3] == 60, "arp up order %d %d %d %d", ons[0], ons[1], ons[2], ons[3]);
    sp_key(60, 0, 0);
    sp_key(64, 0, 0);
    sp_key(67, 0, 0);
    run_us(200000);
    CHECK(sounding_count() == 0, "arp left %d notes after release", sounding_count());
}

static void test_arp_timing_and_gate(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.arp_rate = RATE_1_8;           /* 250 ms at 120 */
    cfg.arp_gate = 50;
    sp_init(&cfg, out);
    sp_key(60, 100, 1);
    /* sample sounding state across one step */
    run_us(100000);
    CHECK(sounding_count() == 1, "note on during first half of gate");
    run_us(50000);                     /* t = 150 ms > 125 ms gate */
    CHECK(sounding_count() == 0, "note off after gate");
    run_us(110000);                    /* t = 260 ms: second step started */
    int ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons == 2, "2 steps after 260 ms, got %d", ons);
}

static void test_arp_hold(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.arp_hold = 1;
    sp_init(&cfg, out);
    sp_key(60, 100, 1);
    sp_key(60, 0, 0);
    nev = 0;
    run_us(500000);
    int ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons >= 3, "held arp keeps playing after release (%d steps)", ons);
    sp_key(62, 100, 1);                /* new gesture replaces latch */
    nev = 0;
    run_us(400000);
    for (int i = 0; i < nev; i++)
        if (ev_on[i])
            CHECK(ev_note[i] == 62, "latched pattern replaced, got %d", ev_note[i]);
}

static void test_ext_clock(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.ext_clock = 1;
    cfg.arp_rate = RATE_1_16;          /* 6 clocks per step */
    sp_init(&cfg, out);
    sp_key(60, 100, 1);
    sp_key(67, 100, 1);
    run_us(1000000);                   /* internal clock must be ignored */
    int ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons == 1, "only the immediate first step without clock, got %d", ons);
    for (int i = 0; i < 12; i++)
        sp_midi_clock();
    ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons == 3, "12 clocks = 2 more steps, got %d", ons);
}

static void test_arp_dedupe(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.chord = CHORD_TRIAD;          /* C and E triads share E and G */
    sp_init(&cfg, out);
    sp_key(60, 100, 1);
    sp_key(64, 100, 1);
    run_us(125000 * 6 + 1000);
    int seen[128] = {0};
    for (int i = 0; i < nev; i++)
        if (ev_on[i])
            seen[ev_note[i]]++;
    /* pattern C E G B (E-major triad E G# B) -> unique 60 64 67 68 71 */
    int dupes = 0, uniq = 0;
    for (int i = 0; i < 128; i++) {
        uniq += seen[i] > 0;
        dupes += seen[i] > 2;
    }
    CHECK(uniq == 5 && dupes == 0, "dedupe: %d unique, %d repeated", uniq, dupes);
}

static void test_arp_toggle_releases(void)
{
    reset();
    sp_key(60, 100, 1);                /* plain note sounding */
    cfg.arp_on = 1;
    sp_config_changed();
    CHECK(sounding_count() == 0, "arp toggle released plain notes");
}

int main(void)
{
    test_passthrough();
    test_quantize();
    test_easy();
    test_chords();
    test_arp_up();
    test_arp_timing_and_gate();
    test_arp_hold();
    test_ext_clock();
    test_arp_toggle_releases();
    test_arp_dedupe();
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures != 0;
}
