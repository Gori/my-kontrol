/* Minimal stdlib.h for programs run in xsim. */
#ifndef XSIM_STDLIB_H
#define XSIM_STDLIB_H
#include <stddef.h>

int abs(int x);
void exit(int status) __attribute__((noreturn));
#endif
