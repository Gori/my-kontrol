/* The few C library functions the host tests use, for running them in xsim. Output goes
   through _sim_putc (test/simrt.s). Not part of the firmware. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void _sim_putc(int c);
void _exit(int status) __attribute__((noreturn));

FILE *stdout, *stderr;

struct out { char *buf; size_t n, len; };

static void put(struct out *o, char c)
{
    if (!o->buf)
        _sim_putc(c);
    else if (o->len + 1 < o->n)
        o->buf[o->len] = c;
    o->len++;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

/* flags - + space 0, width, precision for s, length hh h l ll z; d i u x X c s p % */
static int format(struct out *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            put(o, *f);
            continue;
        }
        int left = 0, plus = 0, space = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (f[1] == '-') left = 1;
            else if (f[1] == '+') plus = 1;
            else if (f[1] == ' ') space = 1;
            else if (f[1] == '0') zero = 1;
            else break;
        }
        if (f[1] == '*') {
            width = va_arg(ap, int);
            f++;
        }
        while (f[1] >= '0' && f[1] <= '9')
            width = width * 10 + *++f - '0';
        if (f[1] == '.') {
            f++;
            prec = 0;
            while (f[1] >= '0' && f[1] <= '9')
                prec = prec * 10 + *++f - '0';
        }
        while (f[1] == 'l' || f[1] == 'h' || f[1] == 'z')
            lng += *++f == 'l';
        char c = *++f, tmp[24];
        const char *s = tmp;
        int len = 0, neg = 0;
        unsigned long long v = 0;
        switch (c) {
        case 'd': case 'i': {
            long long x = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            neg = x < 0;
            v = neg ? -(unsigned long long)x : (unsigned long long)x;
            break;
        }
        case 'u': case 'x': case 'X': case 'p':
            v = c == 'p' ? (uintptr_t)va_arg(ap, void *) : lng >= 2 ? va_arg(ap, unsigned long long)
                : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (s[len] && (prec < 0 || len < prec))
                len++;
            break;
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        default:
            return -1;
        }
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'p') {
            unsigned base = c == 'x' || c == 'X' || c == 'p' ? 16 : 10;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char *p = tmp + sizeof tmp;
            do {
                *--p = dig[v % base];
                v /= base;
            } while (v);
            s = p;
            len = tmp + sizeof tmp - p;
        }
        char sign = neg ? '-' : plus && (c == 'd' || c == 'i') ? '+' : space && (c == 'd' || c == 'i') ? ' ' : 0;
        int fill = width - len - !!sign;
        if (!left && !zero)
            pad(o, ' ', fill);
        if (sign)
            put(o, sign);
        if (!left && zero)
            pad(o, '0', fill);
        for (int i = 0; i < len; i++)
            put(o, s[i]);
        if (left)
            pad(o, ' ', fill);
    }
    return 0;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct out o = { buf, n, 0 };
    format(&o, fmt, ap);
    if (n)
        buf[o.len < n ? o.len : n - 1] = 0;
    return o.len;
}

int vprintf(const char *fmt, va_list ap)
{
    struct out o = { 0, 0, 0 };
    format(&o, fmt, ap);
    return o.len;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int putchar(int c)
{
    _sim_putc(c);
    return c;
}

int puts(const char *s)
{
    while (*s)
        _sim_putc(*s++);
    _sim_putc('\n');
    return 0;
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
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

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (uint8_t)*a - (uint8_t)*b;
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

void exit(int status)
{
    _exit(status);
}
