/* Hardware threads from C (xs1.h). A thread is unsynchronised: it gets its own stack, the
   tile's cp and dp, and frees itself when fn returns. */
#include "xs1.h"

void xs1_thread_exit(void);
asm(".text\n"
    ".globl xs1_thread_exit\n"
    ".cc_top xs1_thread_exit.function, xs1_thread_exit\n"
    "xs1_thread_exit:\n"
    "    freet\n"
    ".cc_bottom xs1_thread_exit.function\n");

uint32_t xs1_thread_start(void (*fn)(uint32_t), uint32_t arg, uint32_t *stack, uint32_t words)
{
    uint32_t t = xs1_getr(RES_THREAD), dp, cp;
    if (!t)
        return 0;
    uint32_t sp = (uint32_t)(stack + words - 2);
    asm volatile("ldaw %0, dp[0]" : "=r"(dp));
    asm volatile("ldaw r11, cp[0]\n\tmov %0, r11" : "=r"(cp) :: "r11");
    asm volatile("init t[%0]:pc, %1" :: "r"(t), "r"(fn));
    asm volatile("init t[%0]:sp, %1" :: "r"(t), "r"(sp));
    asm volatile("init t[%0]:dp, %1" :: "r"(t), "r"(dp));
    asm volatile("init t[%0]:cp, %1" :: "r"(t), "r"(cp));
    asm volatile("init t[%0]:lr, %1" :: "r"(t), "r"(xs1_thread_exit));
    asm volatile("set t[%0]:r0, %1" :: "r"(t), "r"(arg));
    asm volatile("start t[%0]" :: "r"(t));
    return t;
}
