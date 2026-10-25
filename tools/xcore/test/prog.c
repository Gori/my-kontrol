/* Toolchain test program: prints results of code paths that exercise the compiler helpers,
   jump tables, cp/dp data, function pointers and the stack. xtest.py runs it natively and in
   xsim and compares the output; it is also the payload of the test upgrade image. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long long u64;
typedef long long s64;

static volatile uint32_t seed = 12345;        /* volatile: keep the compiler from folding */
static const int primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
static int counter = 7;                       /* dp data */
static uint8_t zeroed[300];                   /* bss, beyond the short dp offsets */
struct pt { int x, y, z; char tag[9]; };

static uint32_t rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed;
}

static u64 rnd64(void)
{
    u64 v = rnd();
    return v << 32 | rnd();
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int sw16(int x)                        /* dense switch: .jmptable */
{
    switch (x) {
    case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44;
    case 4: return 55; case 5: return 66; case 7: return 88; case 9: return 99;
    default: return -1;
    }
}

static int sw40(int x)                        /* more than 32 cases: .jmptable32 */
{
    switch (x) {
    case 0: return 3; case 1: return 1; case 2: return 4; case 3: return 1; case 4: return 5;
    case 5: return 9; case 6: return 2; case 7: return 6; case 8: return 5; case 9: return 3;
    case 10: return 58; case 11: return 97; case 12: return 93; case 13: return 23;
    case 14: return 84; case 15: return 62; case 16: return 64; case 17: return 33;
    case 18: return 83; case 19: return 27; case 20: return 95; case 21: return 2;
    case 22: return 88; case 23: return 41; case 24: return 97; case 25: return 16;
    case 26: return 93; case 27: return 99; case 28: return 37; case 29: return 51;
    case 30: return 5; case 31: return 82; case 32: return 9; case 33: return 74;
    case 34: return 94; case 35: return 45; case 36: return 92; case 37: return 30;
    case 38: return 78; case 39: return 16;
    default: return 0;
    }
}

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int (*const ops[])(int, int) = {add, sub};

int main(void)
{
    printf("fib %d %d\n", fib(10), fib(15));
    for (int i = 0; i < 8; i++) {
        u64 a = rnd64(), b = rnd64() >> (rnd() % 64);
        s64 sa = (s64)a >> (rnd() % 40), sb = (s64)(b | 1) * ((rnd() & 1) ? -1 : 1);
        int s = rnd() % 64;
        printf("u64 %llx %llx: / %llx %% %llx * %llx << %llx >> %llx\n", a, b, b ? a / b : 0,
               b ? a % b : 0, a * b, a << s, a >> s);
        printf("s64 %lld %lld: / %lld %% %lld >> %lld\n", sa, sb, sa / sb, sa % sb, sa >> s);
        int32_t x = (int32_t)rnd(), y = (int32_t)(rnd() >> (rnd() % 31)) | 1;
        printf("s32 %d %d: / %d %% %d u %u %u\n", x, y, x / y, x % y, (uint32_t)x / (uint32_t)y,
               (uint32_t)x % (uint32_t)y);
    }
    int t = 0;
    for (int i = -2; i < 45; i++)
        t = t * 31 + sw16(i) * 7 + sw40(i);
    printf("switch %d\n", t);
    struct pt p = {1, -2, 3, "abcdefgh"}, q;
    q = p;                                    /* __memcpy_4 */
    q.tag[2] = 'X';
    char buf[40];
    memset(buf, '.', sizeof buf);
    memcpy(buf + 3, q.tag, 8);
    memmove(buf + 5, buf + 3, 8);
    buf[20] = 0;
    printf("struct %d %d %d %s %s\n", q.x, q.y, q.z, q.tag, buf);
    int sum = 0;
    for (unsigned i = 0; i < sizeof primes / sizeof *primes; i++)
        sum += ops[i & 1](primes[i], counter++);
    for (unsigned i = 0; i < sizeof zeroed; i++)
        sum += zeroed[i]++;
    printf("data %d %d %d\n", sum, counter, zeroed[299]);
    printf("fmt [%5d] [%-5d] [%05d] [%+d] [%x] [%s] [%c]\n", 42, 42, 42, 42, 0xbeef, "ok", 'z');
    return 0;
}
