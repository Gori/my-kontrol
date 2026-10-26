/* Compiler support for clang --target=xcore: the library functions LLVM's XCore backend calls
   (memcpy, __memcpy_4, memmove, memset, misaligned word access, 64-bit shifts and division),
   and memcmp and strlen for the firmware. Written from scratch.
   Build with -ffreestanding -fno-builtin so these loops do not turn back into calls to
   themselves. 64-bit shifts by a variable amount would call the helpers below, so the
   helpers only shift by constants. */
#include <stddef.h>
#include <stdint.h>

/* chanend 0, allocated by crt0.s before anything else (the tile link uses it) */
uint32_t xs1_chanend0;

typedef unsigned long long u64;
typedef long long s64;

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t *)d = *(const uint32_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

/* memcpy with both pointers word aligned (LLVM emits this for aligned copies) */
void __memcpy_4(void *dst, const void *src, size_t n)
{
    uint32_t *d = dst;
    const uint32_t *s = src;
    for (; n >= 4; n -= 4)
        *d++ = *s++;
    uint8_t *db = (uint8_t *)d;
    const uint8_t *sb = (const uint8_t *)s;
    while (n--)
        *db++ = *sb++;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--)
            d[n] = s[n];
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

/* word loads and stores at addresses that are not word aligned (packed structs) */
uint32_t __misaligned_load(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

void __misaligned_store(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* 64-bit values as two words */
typedef union { u64 v; struct { uint32_t lo, hi; } w; } dw;

u64 __ashldi3(u64 a, int b)
{
    dw x = { a }, r;
    if (b >= 32) {
        r.w.lo = 0;
        r.w.hi = x.w.lo << (b - 32);
    } else if (b) {
        r.w.lo = x.w.lo << b;
        r.w.hi = x.w.hi << b | x.w.lo >> (32 - b);
    } else
        return a;
    return r.v;
}

u64 __lshrdi3(u64 a, int b)
{
    dw x = { a }, r;
    if (b >= 32) {
        r.w.hi = 0;
        r.w.lo = x.w.hi >> (b - 32);
    } else if (b) {
        r.w.hi = x.w.hi >> b;
        r.w.lo = x.w.lo >> b | x.w.hi << (32 - b);
    } else
        return a;
    return r.v;
}

s64 __ashrdi3(s64 a, int b)
{
    dw x = { (u64)a }, r;
    if (b >= 32) {
        r.w.hi = (uint32_t)((int32_t)x.w.hi >> 31);
        r.w.lo = (uint32_t)((int32_t)x.w.hi >> (b - 32));
    } else if (b) {
        r.w.hi = (uint32_t)((int32_t)x.w.hi >> b);
        r.w.lo = x.w.lo >> b | x.w.hi << (32 - b);
    } else
        return a;
    return (s64)r.v;
}

/* restoring division, one quotient bit per step */
static u64 udivmod(u64 n, u64 d, u64 *rem)
{
    dw dd = { d };
    if (!dd.w.hi) {
        dw nn = { n };
        if (!dd.w.lo)
            return *rem = n, ~0ull;     /* division by zero: no trap, all ones */
        if (!nn.w.hi) {
            *rem = nn.w.lo % dd.w.lo;
            return nn.w.lo / dd.w.lo;
        }
    }
    u64 q = 0, r = 0;
    for (int i = 0; i < 64; i++) {
        r = r << 1 | n >> 63;
        n <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    *rem = r;
    return q;
}

u64 __udivdi3(u64 a, u64 b) { u64 r; return udivmod(a, b, &r); }
u64 __umoddi3(u64 a, u64 b) { u64 r; udivmod(a, b, &r); return r; }

s64 __divdi3(s64 a, s64 b)
{
    u64 r, q = udivmod(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return (a < 0) != (b < 0) ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
    u64 r;
    udivmod(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(s64)r : (s64)r;
}
