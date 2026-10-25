/* Minimal stdio for programs run in xsim (tools/xcore/test/simlib.c); output goes to _sim_putc. */
#ifndef XSIM_STDIO_H
#define XSIM_STDIO_H
#include <stdarg.h>
#include <stddef.h>

typedef struct FILE FILE;
extern FILE *stdout, *stderr;

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int fflush(FILE *f);
#endif
