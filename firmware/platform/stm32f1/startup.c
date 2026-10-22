/* STM32F103 behind NI's DFU bootloader: vectors, reset, clocks, watchdog,
   flash, the bootloader handshake and crash recovery.

   Recovery, so a bad build never needs a debugger:
   - a fault or a watchdog reset reboots; three in a row without 10 s of
     stable running in between stop in the bootloader (USB DFU)
   - holding the board's boot combo at power-up stops in the bootloader
   - a clock failure at boot stops in the bootloader */
#include "hw.h"
#include "hal.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
void reset_handler(void);
void mk_main(void);
void usb_irq(void);
void board_tick(void);
static void fault_handler(void);
static void systick_handler(void);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".vectors"), used))
static void (*const vectors[16 + 60])(void) = {
    (void (*)(void))&_estack,
    reset_handler,
    [2 ... 16 + 59] = fault_handler,
    [15] = systick_handler,            /* 10 kHz tick */
    [16 + 19] = usb_irq,               /* USB_HP_CAN1_TX */
    [16 + 20] = usb_irq,               /* USB_LP_CAN1_RX0 */
};
#pragma GCC diagnostic pop

/* Flag page contents understood by the NI bootloader:
   MAGIC     - stay in DFU (written before resetting into DFU)
   APP_VALID (59 95 3A A3 55 5A A5 AA) - written by the bootloader after a
               download; without it a cold boot stays in DFU, so this firmware
               never erases the page except to write MAGIC */
static const uint8_t MAGIC[8] = {0x55, 0x5A, 0xA5, 0xAA, 0x59, 0x95, 0x3A, 0xA3};

/* Backup registers survive a system reset (not a power cut).
   DR1 = boot stage, DR2 = consecutive crash count, DR3 = boot counter. */
#define BKP_DR1 REG(0x40006C04)
#define BKP_DR2 REG(0x40006C08)
#define BKP_DR3 REG(0x40006C0C)
#define CRASH_LIMIT 3
#define STABLE_MS 10000

uint16_t prev_stage, prev_boots, crash_count;
uint32_t reset_flags;                          /* RCC_CSR at reset */

static volatile uint32_t ms_count;
static uint8_t tick_div;

static void stage(uint16_t s)
{
    BKP_DR1 = s;
}

/* ---------- time ---------- */

static void systick_handler(void)
{
    board_tick();
    if (++tick_div >= 10) {
        tick_div = 0;
        ms_count++;
    }
}

uint32_t hal_ms(void)
{
    return ms_count;
}

void hal_watchdog_feed(void)
{
    IWDG_KR = 0xAAAA;
}

void hal_delay_ms(uint32_t ms)
{
    /* ~8000 iterations per ms at 72 MHz; only for coarse waits */
    for (volatile uint32_t i = ms * 8000; i; i--)
        IWDG_KR = 0xAAAA;
}

/* ~2 s hardware watchdog (LSI ~40 kHz / 64 / 1250), fed by the main loop. */
static void watchdog_start(void)
{
    IWDG_KR = 0x5555;
    REG(0x40003004) = 4;       /* IWDG_PR: /64 */
    REG(0x40003008) = 1250;    /* IWDG_RLR */
    IWDG_KR = 0xAAAA;
    IWDG_KR = 0xCCCC;
}

/* ---------- flash ---------- */

static void flash_unlock(void)
{
    if (FLASH_CR & (1 << 7)) {
        FLASH_KEYR = 0x45670123;
        FLASH_KEYR = 0xCDEF89AB;
    }
}

static int flash_wait(void)
{
    while (FLASH_SR & 1)
        ;
    int err = FLASH_SR & 0x14;         /* WRPRTERR, PGERR */
    FLASH_SR = 0x34;
    return err ? -1 : 0;
}

/* Only the data pages below the bootloader flag page may be written. */
static int writable(uint32_t addr, uint32_t len)
{
    return addr >= FLASH_PRESETS_A && addr + len <= FLASH_SETTINGS_B + FLASH_PAGE;
}

static int erase_page(uint32_t addr)
{
    flash_unlock();
    flash_wait();
    FLASH_CR |= 1 << 1;
    FLASH_AR = addr;
    FLASH_CR |= 1 << 6;
    int r = flash_wait();
    FLASH_CR &= ~(1u << 1);
    FLASH_CR |= 1 << 7;
    return r;
}

static int program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    int r = 0;
    flash_unlock();
    for (uint32_t i = 0; i < len && !r; i += 2) {
        FLASH_CR |= 1 << 0;
        *(volatile uint16_t *)(addr + i) = src[i] | (i + 1 < len ? src[i + 1] : 0xFF) << 8;
        r = flash_wait();
        FLASH_CR &= ~1u;
    }
    FLASH_CR |= 1 << 7;
    return r;
}

int hal_flash_erase(uintptr_t addr)
{
    if (!writable(addr, FLASH_PAGE))
        return -1;
    IWDG_KR = 0xAAAA;
    return erase_page(addr);
}

int hal_flash_write(uintptr_t addr, const void *src, uint32_t len)
{
    if (!writable(addr, len))
        return -1;
    IWDG_KR = 0xAAAA;
    return program(addr, src, len);
}

void hal_unique_id(uint8_t out[12])
{
    for (int i = 0; i < 12; i++)
        out[i] = ((const volatile uint8_t *)0x1FFFF7E8)[i];
}

/* ---------- bootloader ---------- */

static int flag_page_is(const uint8_t *v)
{
    const volatile uint8_t *p = (const volatile uint8_t *)BOOT_FLAG_ADDR;
    for (int i = 0; i < 8; i++)
        if (p[i] != v[i])
            return 0;
    return 1;
}

static void flag_page_write(const uint8_t *v)
{
    if (flag_page_is(v))
        return;
    erase_page(BOOT_FLAG_ADDR);
    program(BOOT_FLAG_ADDR, v, 8);
}

int boot_flag_armed(void)
{
    return flag_page_is(MAGIC);
}

/* Same sequence as NI's firmware: write the flag, drop the USB pull-up, reset. */
void hal_enter_bootloader(void)
{
    __asm volatile("cpsid i");
    stage(0xB0);
    flag_page_write(MAGIC);
    GPIOA_BRR = 1 << 8;
    hal_delay_ms(50);
    SCB_AIRCR = 0x05FA0004;
    for (;;) ;
}

void hal_reboot(void)
{
    __asm volatile("cpsid i");
    GPIOA_BRR = 1 << 8;
    hal_delay_ms(50);
    SCB_AIRCR = 0x05FA0004;
    for (;;) ;
}

void hal_mark_stable(void)
{
    BKP_DR2 = 0;
}

static void fault_handler(void)
{
    stage(0xFF);
    BKP_DR2 = crash_count + 1;
    if (crash_count + 1 >= CRASH_LIMIT)
        hal_enter_bootloader();
    hal_reboot();
}

/* ---------- clocks ---------- */

static int clock_init(void)
{
    RCC_CR |= 1;                       /* HSI on */
    while (!(RCC_CR & (1 << 1)))
        ;
    RCC_CFGR &= ~3u;                   /* SYSCLK = HSI, in case the bootloader left PLL selected */
    while (RCC_CFGR & (3 << 2))
        ;
    RCC_CR &= ~(1u << 24);             /* PLL off */
    while (RCC_CR & (1 << 25))
        ;
    RCC_CR |= 1 << 16;                 /* HSE (8 MHz crystal) on */
    for (uint32_t t = 0; !(RCC_CR & (1 << 17)); t++)
        if (t > 500000)
            return 0;
    FLASH_ACR = 0x12;                  /* prefetch, 2 wait states */
    /* PLL = HSE x9 = 72 MHz, USB = PLL/1.5 = 48 MHz, APB1 /2, ADC /6 */
    RCC_CFGR = 0x001D8400;
    RCC_CR |= 1 << 24;
    while (!(RCC_CR & (1 << 25)))
        ;
    RCC_CFGR |= 2;
    while (((RCC_CFGR >> 2) & 3) != 2)
        ;
    return 1;
}

void hal_usb_init(void)
{
    RCC_APB1ENR |= 1 << 23;            /* USB clock; USBPRE was set in clock_init */
    RCC_APB1RSTR |= 1 << 23;           /* drop whatever state the bootloader left */
    RCC_APB1RSTR &= ~(1u << 23);
}

void hal_tick_start(void)
{
    SYST_RVR = 72 * 100 - 1;           /* 100 us from HCLK */
    SYST_CVR = 0;
    SYST_CSR = 7;                      /* HCLK, interrupt, enable */
}

void reset_handler(void)
{
    __asm volatile("cpsid i");
    SYST_CSR = 0;
    SCB_VTOR = 0x08003000;

    uint32_t *s = &_sidata, *d = &_sdata;
    while (d < &_edata)
        *d++ = *s++;
    for (d = &_sbss; d < &_ebss; d++)
        *d = 0;

    RCC_APB1ENR |= (1 << 27) | (1 << 28);          /* BKP, PWR clocks */
    REG(0x40007000) |= 1 << 8;                     /* PWR_CR.DBP: backup domain writable */
    prev_stage = BKP_DR1;
    prev_boots = BKP_DR3;
    BKP_DR3 = prev_boots + 1;
    crash_count = BKP_DR2;
    stage(1);

    uint32_t csr = REG(0x40021024);
    REG(0x40021024) |= 1 << 24;                    /* RCC_CSR.RMVF: clear reset flags */
    reset_flags = csr;
    if (csr & (1u << 29))                          /* the previous run hung */
        BKP_DR2 = ++crash_count;
    if (csr & (1u << 27))                          /* power-on reset: fresh start */
        BKP_DR2 = crash_count = 0;
    if (crash_count >= CRASH_LIMIT) {
        BKP_DR2 = 0;
        hal_enter_bootloader();
    }
    watchdog_start();
    stage(2);

    if (!clock_init()) {
        stage(0xE1);
        hal_enter_bootloader();
    }
    stage(3);

    RCC_APB2ENR |= (1 << 0) | (1 << 2);            /* AFIO, GPIOA */
    GPIOA_CRH = (GPIOA_CRH & ~0xFu) | 0x2;         /* PA8 push-pull output: USB pull-up */
    GPIOA_BRR = 1 << 8;                            /* make the host re-enumerate */

    board_early_init();
    if (board_boot_combo())
        hal_enter_bootloader();

    /* the bootloader may leave IRQs enabled or pending */
    for (int i = 0; i < 3; i++) {
        REG(0xE000E180 + 4 * i) = 0xFFFFFFFF;      /* NVIC_ICER */
        REG(0xE000E280 + 4 * i) = 0xFFFFFFFF;      /* NVIC_ICPR */
    }
    stage(4);
    mk_main();
}
