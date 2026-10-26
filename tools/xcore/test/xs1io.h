/* XS1 resource instructions as C inline functions, for the toolchain test programs. */
#ifndef XS1IO_H
#define XS1IO_H
#include <stdint.h>

enum { RES_TIMER = 1, RES_CHANEND = 2 };
enum { SETC_INUSE_ON = 0x8, SETC_COND_AFTER = 0x9, SETC_INV = 0x600f };
enum { CT_END = 1 };

static inline uint32_t getr(uint32_t type) { uint32_t r; asm volatile("getr %0, %1" : "=r"(r) : "n"(type)); return r; }
static inline void setc(uint32_t r, uint32_t v) { asm volatile("setc res[%0], %1" :: "r"(r), "r"(v)); }
static inline void setd(uint32_t r, uint32_t v) { asm volatile("setd res[%0], %1" :: "r"(r), "r"(v)); }
static inline void setpt(uint32_t p, uint32_t t) { asm volatile("setpt res[%0], %1" :: "r"(p), "r"(t)); }
static inline uint32_t getts(uint32_t p) { uint32_t v; asm volatile("getts %0, res[%1]" : "=r"(v) : "r"(p)); return v; }
static inline void out(uint32_t r, uint32_t v) { asm volatile("out res[%0], %1" :: "r"(r), "r"(v) : "memory"); }
static inline uint32_t in(uint32_t r) { uint32_t v; asm volatile("in %0, res[%1]" : "=r"(v) : "r"(r) : "memory"); return v; }
static inline void outt(uint32_t c, uint32_t v) { asm volatile("outt res[%0], %1" :: "r"(c), "r"(v) : "memory"); }
static inline uint32_t intok(uint32_t c) { uint32_t v; asm volatile("int %0, res[%1]" : "=r"(v) : "r"(c) : "memory"); return v; }
static inline void outct(uint32_t c, uint32_t v) { asm volatile("outct res[%0], %1" :: "r"(c), "r"(v) : "memory"); }
static inline void chkct(uint32_t c, uint32_t v) { asm volatile("chkct res[%0], %1" :: "r"(c), "r"(v) : "memory"); }

/* wait until the 100 MHz reference timer passes t */
static inline void wait_until(uint32_t tmr, uint32_t t)
{
    setd(tmr, t);
    setc(tmr, SETC_COND_AFTER);
    (void)in(tmr);
}

/* tile link packets (contract: HostLink in tools/xcore/s49emu.py) */
enum { LINK_USB_MIDI_TX = 0x81, LINK_DIN_TX,
       LINK_USB_STATE = 0x01, LINK_USB_MIDI_RX, LINK_DIN_RX };
extern uint32_t xs1_chanend0;    /* chanend 0, allocated by crt0.s (rt.c) */
#define LINK_TILE0 0x80000002u      /* chanend 0 of each tile */
#define LINK_TILE1 0x00000002u

static inline void link_send(uint32_t c, uint32_t type, const uint8_t *p, uint32_t n)
{
    outt(c, type);
    outt(c, n);
    for (uint32_t i = 0; i < n; i++)
        outt(c, p[i]);
    outct(c, CT_END);
}

/* receive one packet into p (up to 255 bytes); returns the type, *n = payload length */
static inline uint32_t link_recv(uint32_t c, uint8_t *p, uint32_t *n)
{
    uint32_t type = intok(c);
    *n = intok(c);
    for (uint32_t i = 0; i < *n; i++)
        p[i] = (uint8_t)intok(c);
    chkct(c, CT_END);
    return type;
}
#endif
