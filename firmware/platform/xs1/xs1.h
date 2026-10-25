#pragma once
/* XS1 resources from C: the instructions as inline functions, threads, timers and a
   two-way event wait. Resource IDs and setc values: the XS1 architecture. */
#include <stdint.h>

enum { RES_PORT = 0, RES_TIMER = 1, RES_CHANEND = 2, RES_THREAD = 4 };
enum {
    SETC_INUSE_ON = 0x8, SETC_COND_FULL = 0x1, SETC_COND_AFTER = 0x9, SETC_COND_EQ = 0x11,
    SETC_INVERT = 0x600f, SETC_BUFFERS = 0x200f, SETC_CLOCKPORT = 0x500f, SETC_START = 0xf,
};
#define CLKBLK(n) ((uint32_t)(n) << 8 | 6)
#define CLK_REF 1                   /* setclk source: the 100 MHz reference clock */
enum { CT_END = 1 };

/* port IDs: width << 16 | index << 8 (XS1_PORT_1A = 0x10000 ... XS1_PORT_4A = 0x40000) */
#define PORT_1(letter) (0x10000u | (uint32_t)((letter) - 'A') << 8)
#define PORT_4(letter) (0x40000u | (uint32_t)((letter) - 'A') << 8)

#define TICKS_PER_MS 100000u        /* 100 MHz reference clock */
#define TICKS_PER_US 100u

#define xs1_getr(type) ({ uint32_t r_; asm volatile("getr %0, %1" : "=r"(r_) : "n"(type)); r_; })
static inline void xs1_setc(uint32_t r, uint32_t v) { asm volatile("setc res[%0], %1" :: "r"(r), "r"(v)); }
static inline void xs1_setd(uint32_t r, uint32_t v) { asm volatile("setd res[%0], %1" :: "r"(r), "r"(v)); }
static inline void xs1_setclk(uint32_t r, uint32_t src) { asm volatile("setclk res[%0], %1" :: "r"(r), "r"(src)); }
static inline void xs1_settw(uint32_t p, uint32_t w) { asm volatile("settw res[%0], %1" :: "r"(p), "r"(w)); }
static inline void xs1_syncr(uint32_t p) { asm volatile("syncr res[%0]" :: "r"(p) : "memory"); }
static inline void xs1_setpt(uint32_t p, uint32_t t) { asm volatile("setpt res[%0], %1" :: "r"(p), "r"(t)); }
static inline uint32_t xs1_getts(uint32_t p)
{
    uint32_t v;
    asm volatile("getts %0, res[%1]" : "=r"(v) : "r"(p));
    return v;
}
static inline void xs1_out(uint32_t r, uint32_t v) { asm volatile("out res[%0], %1" :: "r"(r), "r"(v) : "memory"); }
static inline uint32_t xs1_in(uint32_t r)
{
    uint32_t v;
    asm volatile("in %0, res[%1]" : "=r"(v) : "r"(r) : "memory");
    return v;
}
static inline void xs1_outt(uint32_t c, uint32_t v) { asm volatile("outt res[%0], %1" :: "r"(c), "r"(v) : "memory"); }
static inline uint32_t xs1_int(uint32_t c)
{
    uint32_t v;
    asm volatile("int %0, res[%1]" : "=r"(v) : "r"(c) : "memory");
    return v;
}
static inline void xs1_outct(uint32_t c, uint32_t v) { asm volatile("outct res[%0], %1" :: "r"(c), "r"(v) : "memory"); }
static inline void xs1_chkct(uint32_t c, uint32_t v) { asm volatile("chkct res[%0], %1" :: "r"(c), "r"(v) : "memory"); }

/* the reference timer, and waiting on it with a timer resource of this thread */
static inline uint32_t xs1_time(uint32_t tmr) { return xs1_in(tmr); }
static inline void xs1_wait_until(uint32_t tmr, uint32_t t)
{
    xs1_setd(tmr, t);
    xs1_setc(tmr, SETC_COND_AFTER);
    (void)xs1_in(tmr);
    xs1_setc(tmr, SETC_COND_FULL);
}

/* 1 if chanend c has a token waiting, without blocking (events polled with setsr/clrsr) */
static inline int xs1_ready(uint32_t c)
{
    int r;
    asm volatile(
        "clre\n"
        "ldap r11, .Lxr%=\n"
        "setv res[%1], r11\n"
        "eeu res[%1]\n"
        "ldc %0, 0\n"
        "setsr 1\n"
        "clrsr 1\n"
        "bu .Lxd%=\n"
        ".Lxr%=:\n"
        "ldc %0, 1\n"
        ".Lxd%=:\n"
        "edu res[%1]\n"
        : "=&r"(r) : "r"(c) : "r11", "memory");
    return r;
}

/* wait until chanend a or b has a token: 0 = a, 1 = b */
static inline int xs1_wait2(uint32_t a, uint32_t b)
{
    int r;
    asm volatile(
        "clre\n"
        "ldap r11, .Lwa%=\n"
        "setv res[%1], r11\n"
        "eeu res[%1]\n"
        "ldap r11, .Lwb%=\n"
        "setv res[%2], r11\n"
        "eeu res[%2]\n"
        "waiteu\n"
        ".Lwa%=:\n"
        "ldc %0, 0\n"
        "bu .Lwd%=\n"
        ".Lwb%=:\n"
        "ldc %0, 1\n"
        ".Lwd%=:\n"
        "clre\n"
        : "=&r"(r) : "r"(a), "r"(b) : "r11", "memory");
    return r;
}

/* wait until chanend c has a token or timer tmr passes t: 0 = chanend, 1 = time */
static inline int xs1_wait_chan_until(uint32_t c, uint32_t tmr, uint32_t t)
{
    int r;
    asm volatile(
        "clre\n"
        "ldap r11, .Lca%=\n"
        "setv res[%1], r11\n"
        "eeu res[%1]\n"
        "setd res[%2], %3\n"
        "setc res[%2], 9\n"
        "ldap r11, .Lcb%=\n"
        "setv res[%2], r11\n"
        "eeu res[%2]\n"
        "waiteu\n"
        ".Lca%=:\n"
        "ldc %0, 0\n"
        "bu .Lcd%=\n"
        ".Lcb%=:\n"
        "ldc %0, 1\n"
        ".Lcd%=:\n"
        "clre\n"
        "setc res[%2], 1\n"
        : "=&r"(r) : "r"(c), "r"(tmr), "r"(t) : "r11", "memory");
    return r;
}

/* a clock block dividing the reference clock: 100 MHz / (2 * div); attach ports, then start */
static inline void xs1_clock_init(uint32_t cb, uint32_t div)
{
    xs1_setc(cb, SETC_INUSE_ON);
    xs1_setclk(cb, CLK_REF);
    xs1_setd(cb, div);
}

/* start fn(arg) on a new hardware thread of this tile with the given stack; 0 if none free */
uint32_t xs1_thread_start(void (*fn)(uint32_t), uint32_t arg, uint32_t *stack, uint32_t words);
