/* XS1 (XMOS xCORE, Komplete Kontrol MK1) platform: entry from crt0.s. The hal.h functions for
   this platform are not written yet; tools/xcore/xtest.py links the core with them stubbed. */
void mk_main(void);

int main(void)
{
    mk_main();
    return 0;
}
