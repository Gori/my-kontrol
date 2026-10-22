/* Host tests for core/smartplay.c (make test) */
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

static const char *current;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s %s:%d: ", current, __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static sp_config_t cfg;

/* the key ID is the played note unless a test says otherwise */
#define K(n, v, on) sp_key((n), (n), (v), (on))

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
    K(60, 100, 1);
    CHECK(nev == 1 && ev_note[0] == 60 && ev_vel[0] == 100 && ev_on[0], "passthrough on");
    K(60, 0, 0);
    CHECK(nev == 2 && ev_note[1] == 60 && !ev_on[1], "passthrough off");
}

static void test_quantize(void)
{
    reset();
    cfg.scale_mode = SCALE_QUANTIZE;   /* C major */
    K(61, 100, 1);                /* C# -> C (tie goes down) */
    CHECK(ev_note[0] == 60, "C# quantized to %d", ev_note[0]);
    K(61, 0, 0);
    CHECK(ev_note[1] == 60 && !ev_on[1], "release of quantized note");
    cfg.root = 2;                      /* D major: F# in, F out */
    nev = 0;
    K(65, 100, 1);
    CHECK(ev_note[0] == 64, "F in D major -> %d", ev_note[0]);
}

static void test_easy(void)
{
    reset();
    cfg.scale_mode = SCALE_EASY;
    cfg.scale = 1;                     /* C minor */
    K(64, 100, 1);                /* E (3rd white key) -> Eb */
    CHECK(ev_note[0] == 63, "easy E in C minor -> %d", ev_note[0]);
    nev = 0;
    K(61, 100, 1);                /* black key ignored */
    CHECK(nev == 0, "black key in easy mode produced %d events", nev);
    reset();
    cfg.scale_mode = SCALE_EASY;
    cfg.scale = 9;                     /* major pentatonic, 5 notes */
    K(60, 100, 1);
    K(71, 100, 1);                /* B = 7th white key -> wraps to D one octave up */
    CHECK(ev_note[0] == 60 && ev_note[1] == 74, "pentatonic easy: %d %d", ev_note[0], ev_note[1]);
}

static void test_chords(void)
{
    reset();
    cfg.chord = CHORD_TRIAD;           /* no scale: major triad */
    K(60, 90, 1);
    CHECK(nev == 3 && ev_note[0] == 60 && ev_note[1] == 64 && ev_note[2] == 67, "C triad");
    K(60, 0, 0);
    CHECK(sounding_count() == 0, "triad fully released");

    reset();
    cfg.scale_mode = SCALE_QUANTIZE;
    cfg.chord = CHORD_TRIAD;           /* diatonic: D in C major -> D F A */
    K(62, 90, 1);
    CHECK(ev_note[0] == 62 && ev_note[1] == 65 && ev_note[2] == 69, "diatonic Dm: %d %d %d", ev_note[0], ev_note[1], ev_note[2]);

    /* changing chord type while held still releases what was played */
    cfg.chord = CHORD_SEVENTH;
    sp_config_changed();
    K(62, 0, 0);
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
    K(60, 100, 1);
    K(64, 100, 1);
    K(67, 100, 1);
    run_us(125000 * 4 + 1000);
    int ons[16], n = 0;
    for (int i = 0; i < nev; i++)
        if (ev_on[i] && n < 16)
            ons[n++] = ev_note[i];
    CHECK(n >= 5, "arp steps: %d", n);
    CHECK(ons[0] == 60 && ons[1] == 64 && ons[2] == 67 && ons[3] == 60, "arp up order %d %d %d %d", ons[0], ons[1], ons[2], ons[3]);
    K(60, 0, 0);
    K(64, 0, 0);
    K(67, 0, 0);
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
    K(60, 100, 1);
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
    K(60, 100, 1);
    K(60, 0, 0);
    nev = 0;
    run_us(500000);
    int ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons >= 3, "held arp keeps playing after release (%d steps)", ons);
    K(62, 100, 1);                /* new gesture replaces latch */
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
    K(60, 100, 1);
    K(67, 100, 1);
    run_us(1000000);                   /* internal clock must be ignored */
    int ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons == 0, "no steps until the external clock runs, got %d", ons);
    for (int i = 0; i < 12; i++)
        sp_midi_clock();
    ons = 0;
    for (int i = 0; i < nev; i++)
        ons += ev_on[i];
    CHECK(ons == 2, "12 clocks = 2 steps, got %d", ons);
}

static void test_arp_dedupe(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.chord = CHORD_TRIAD;          /* C and E triads share E and G */
    sp_init(&cfg, out);
    K(60, 100, 1);
    K(64, 100, 1);
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
    K(60, 100, 1);                /* plain note sounding */
    cfg.arp_on = 1;
    sp_config_changed();
    CHECK(sounding_count() == 0, "arp toggle released plain notes");
}


static int count_ons(void)
{
    int n = 0;
    for (int i = 0; i < nev; i++)
        n += ev_on[i];
    return n;
}

/* the note-off follows the key, not the note it would play now */
static void test_key_id_release(void)
{
    reset();
    sp_key(5, 60, 100, 1);             /* key 5 plays C */
    sp_key(5, 72, 0, 0);               /* octave changed before release */
    CHECK(sounding_count() == 0, "key-id release left %d notes", sounding_count());
    reset();
    cfg.scale_mode = SCALE_QUANTIZE;
    sp_key(1, 61, 100, 1);
    cfg.root = 5;                      /* scale changed while held */
    sp_config_changed();
    sp_key(1, 61, 0, 0);
    CHECK(sounding_count() == 0, "scale change mid-note left %d", sounding_count());
}

static void test_chord_memory(void)
{
    reset();
    K(60, 100, 1);
    K(63, 100, 1);
    K(67, 100, 1);
    K(70, 100, 1);
    CHECK(sp_learn_chord() == 0, "learn ok");
    CHECK(cfg.chord_mem_n == 4 && cfg.chord_mem[1] == 3 && cfg.chord_mem[3] == 10, "learned m7");
    K(60, 0, 0); K(63, 0, 0); K(67, 0, 0); K(70, 0, 0);
    cfg.chord = CHORD_MEMORY;
    nev = 0;
    K(62, 90, 1);
    CHECK(nev == 4 && ev_note[0] == 62 && ev_note[1] == 65 && ev_note[3] == 72, "D m7 from memory");
    K(62, 0, 0);
    CHECK(sounding_count() == 0, "memory chord released");
    reset();
    K(60, 100, 1);
    CHECK(sp_learn_chord() != 0, "one key is not a chord");
}

static void test_strum(void)
{
    reset();
    cfg.chord = CHORD_TRIAD;
    cfg.strum_ms = 30;
    K(60, 100, 1);
    CHECK(nev == 1 && ev_note[0] == 60, "strum: first note at once (%d)", nev);
    run_us(15000);
    CHECK(nev == 2 && ev_note[1] == 64, "strum: second at 15 ms (%d)", nev);
    run_us(15000);
    CHECK(nev == 3 && ev_note[2] == 67, "strum: third at 30 ms (%d)", nev);
    K(60, 0, 0);
    CHECK(sounding_count() == 0, "strum released");
    /* release mid-strum: only what was sent is released, nothing more arrives */
    reset();
    cfg.chord = CHORD_TRIAD;
    cfg.strum_ms = 100;
    cfg.strum_dir = STRUM_DOWN;
    K(60, 100, 1);
    CHECK(ev_note[0] == 67, "strum down starts high (%d)", ev_note[0]);
    K(60, 0, 0);
    run_us(200000);
    CHECK(sounding_count() == 0 && nev == 2, "early release: %d events", nev);
}

static void test_repeat(void)
{
    reset();
    cfg.repeat_on = 1;
    cfg.arp_rate = RATE_1_8;           /* 250 ms */
    sp_init(&cfg, out);
    K(60, 100, 1);
    K(64, 100, 1);
    run_us(1000000);
    int c = 0, e = 0;
    for (int i = 0; i < nev; i++)
        if (ev_on[i]) { c += ev_note[i] == 60; e += ev_note[i] == 64; }
    CHECK(c >= 4 && e >= 4, "repeat retriggers both notes (%d, %d)", c, e);
    cfg.repeat_vel = REPEAT_VEL_STRIP;
    sp_set_expression(33);
    int mark = nev;
    run_us(260000);
    int ok = 0;
    for (int i = mark; i < nev; i++)
        if (ev_on[i]) ok += ev_vel[i] == 33;
    CHECK(ok >= 2, "repeat velocity from strip (%d)", ok);
    K(60, 0, 0); K(64, 0, 0);
    run_us(300000);
    CHECK(sounding_count() == 0, "repeat released");
}

static void test_hold_off_releases(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.arp_hold = 1;
    sp_init(&cfg, out);
    K(60, 100, 1);
    K(60, 0, 0);
    run_us(300000);
    cfg.arp_hold = 0;
    sp_config_changed();
    nev = 0;
    run_us(500000);
    CHECK(count_ons() == 0, "hold off stops latched notes (%d ons)", count_ons());
}

static void test_ext_stop_then_internal(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.ext_clock = 1;
    sp_init(&cfg, out);
    sp_midi_start();
    sp_midi_stop();
    cfg.ext_clock = 0;                 /* back to the internal clock */
    sp_config_changed();
    K(60, 100, 1);
    run_us(600000);
    CHECK(count_ons() >= 3, "internal clock runs after external stop (%d)", count_ons());
}

static void test_ext_clock_lost(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.ext_clock = 1;
    cfg.arp_gate = 100;
    sp_init(&cfg, out);
    K(60, 100, 1);
    for (int i = 0; i < 7; i++)
        sp_midi_clock();
    run_us(600000);                    /* clock vanished without Stop */
    CHECK(sounding_count() == 0, "notes released when the clock disappears");
}

static void test_continue_keeps_position(void)
{
    reset();
    cfg.arp_on = 1;
    cfg.ext_clock = 1;
    sp_init(&cfg, out);
    sp_midi_start();
    K(60, 100, 1);
    K(64, 100, 1);
    K(67, 100, 1);
    for (int i = 0; i < 6; i++)
        sp_midi_clock();
    sp_midi_stop();
    nev = 0;
    sp_midi_continue();
    for (int i = 0; i < 6; i++)
        sp_midi_clock();
    int last = -1;
    for (int i = 0; i < nev; i++)
        if (ev_on[i]) last = ev_note[i];
    CHECK(last != 60, "continue does not restart the pattern (%d)", last);
}

int main(void)
{
    current = "test_passthrough"; test_passthrough();
    current = "test_quantize"; test_quantize();
    current = "test_easy"; test_easy();
    current = "test_chords"; test_chords();
    current = "test_arp_up"; test_arp_up();
    current = "test_arp_timing_and_gate"; test_arp_timing_and_gate();
    current = "test_arp_hold"; test_arp_hold();
    current = "test_ext_clock"; test_ext_clock();
    current = "test_arp_toggle_releases"; test_arp_toggle_releases();
    current = "test_arp_dedupe"; test_arp_dedupe();
    current = "test_key_id_release"; test_key_id_release();
    current = "test_chord_memory"; test_chord_memory();
    current = "test_strum"; test_strum();
    current = "test_repeat"; test_repeat();
    current = "test_hold_off_releases"; test_hold_off_releases();
    current = "test_ext_stop_then_internal"; test_ext_stop_then_internal();
    current = "test_ext_clock_lost"; test_ext_clock_lost();
    current = "test_continue_keeps_position"; test_continue_keeps_position();
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures != 0;
}
