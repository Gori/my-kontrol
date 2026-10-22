#include "hw.h"
#include "display.h"
#include "splash.h"
#include "app.h"
#include "input.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
void reset_handler(void);
static void fault_handler(void);

/* Every exception except the USB IRQs resets the chip. While the boot flag is
   armed that lands in the NI bootloader, so a crash during bring-up stays
   recoverable over USB. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".vectors"), used))
static void (*const vectors[16 + 60])(void) = {
    (void (*)(void))&_estack,
    reset_handler,
    [2 ... 16 + 59] = fault_handler,
    [15] = input_systick_isr,          /* SysTick: 10 kHz input scan */
    [16 + 19] = usb_irq,               /* USB_HP_CAN1_TX */
    [16 + 20] = usb_irq,               /* USB_LP_CAN1_RX0 */
};
#pragma GCC diagnostic pop

/* Flag page contents understood by the NI bootloader:
   MAGIC     - stay in DFU (written by the app before resetting into DFU)
   APP_VALID - written by the bootloader after a successful download; without
               it a cold boot stays in DFU, so "disarm" must restore it */
static const uint8_t MAGIC[8] = {0x55, 0x5A, 0xA5, 0xAA, 0x59, 0x95, 0x3A, 0xA3};
static const uint8_t APP_VALID[8] = {0x59, 0x95, 0x3A, 0xA3, 0x55, 0x5A, 0xA5, 0xAA};

/* Boot breadcrumbs in backup registers: they survive a system reset (not a
   power cut), so a run that ends in the bootloader can be inspected by the
   next one. prev_* hold the values found at this boot. */
#define BKP_DR1 REG(0x40006C04)
#define BKP_DR2 REG(0x40006C08)
uint16_t prev_stage, prev_boots;
uint8_t flag_page_snapshot[32];                    /* flag page as found at reset */
uint32_t reset_flags;                              /* RCC_CSR at reset */

static void stage(uint16_t s)
{
    BKP_DR1 = s;
}

/* Any fault drops into the bootloader, so a crashing build stays recoverable. */
static void fault_handler(void)
{
    stage(0xFF);
    enter_bootloader();
}

/* ~2 s hardware watchdog (LSI ~40 kHz / 64 / 1250); fed by the main loop and
   delay_ms. A hang resets the chip and the next boot goes to the bootloader. */
static void watchdog_start(void)
{
    IWDG_KR = 0x5555;
    REG(0x40003004) = 4;       /* IWDG_PR: /64 */
    REG(0x40003008) = 1250;    /* IWDG_RLR */
    IWDG_KR = 0xAAAA;
    IWDG_KR = 0xCCCC;
}

void delay_ms(uint32_t ms)
{
    /* ~1 ms per 8000 iterations at 72 MHz; only used for coarse waits */
    for (volatile uint32_t i = ms * 8000; i; i--)
        IWDG_KR = 0xAAAA;
}

static void flash_unlock(void)
{
    if (FLASH_CR & (1 << 7)) {
        FLASH_KEYR = 0x45670123;
        FLASH_KEYR = 0xCDEF89AB;
    }
}

static void flash_wait(void)
{
    while (FLASH_SR & 1)
        ;
    FLASH_SR = 0x34; /* clear EOP, WRPRTERR, PGERR */
}

void flash_erase_page(uint32_t addr)
{
    flash_unlock();
    flash_wait();
    FLASH_CR |= 1 << 1;
    FLASH_AR = addr;
    FLASH_CR |= 1 << 6;
    flash_wait();
    FLASH_CR &= ~(1u << 1);
}

void flash_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    flash_unlock();
    for (uint32_t i = 0; i < len; i += 2) {
        FLASH_CR |= 1 << 0;
        *(volatile uint16_t *)(addr + i) = src[i] | src[i + 1] << 8;
        flash_wait();
        FLASH_CR &= ~1u;
    }
    FLASH_CR |= 1 << 7;
}

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
    flash_erase_page(BOOT_FLAG_ADDR);
    flash_program(BOOT_FLAG_ADDR, v, 8);
}

int boot_flag_armed(void)
{
    return flag_page_is(MAGIC);
}

void boot_flag_arm(void)
{
    flag_page_write(MAGIC);
}

void boot_flag_disarm(void)
{
    flag_page_write(APP_VALID);
}

/* Same sequence as NI's firmware: write flag, drop the USB pull-up, reset. */
void enter_bootloader(void)
{
    boot_flag_arm();
    GPIOA_BRR = 1 << 8;
    delay_ms(50);
    SCB_AIRCR = 0x05FA0004;
    for (;;) ;
}

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
    prev_boots = BKP_DR2;
    BKP_DR2 = prev_boots + 1;
    stage(1);

    for (int i = 0; i < 32; i++)
        flag_page_snapshot[i] = ((const volatile uint8_t *)BOOT_FLAG_ADDR)[i];

    /* A watchdog reset means the previous run hung: go to the bootloader. */
    uint32_t csr = REG(0x40021024);
    REG(0x40021024) |= 1 << 24;                    /* RCC_CSR.RMVF: clear reset flags */
    reset_flags = csr;
    if (csr & (1u << 29))
        enter_bootloader();
    watchdog_start();
    stage(2);

    if (!clock_init()) {
        stage(0xE1);
        enter_bootloader();
    }
    stage(3);

    display_init();
    display_blit(splash);
    display_on();
    delay_ms(1500);


    RCC_APB2ENR |= (1 << 0) | (1 << 2);           /* AFIO, GPIOA */
    GPIOA_CRH = (GPIOA_CRH & ~0xFu) | 0x2;         /* PA8 push-pull output: USB pull-up */
    GPIOA_BRR = 1 << 8;                            /* force the host to re-enumerate */
    delay_ms(20);

    /* The bootloader may leave IRQs enabled or pending; those would hit
       fault_handler as soon as usb_init enables interrupts. */
    for (int i = 0; i < 3; i++) {
        REG(0xE000E180 + 4 * i) = 0xFFFFFFFF;      /* NVIC_ICER */
        REG(0xE000E280 + 4 * i) = 0xFFFFFFFF;      /* NVIC_ICPR */
    }

    usb_init();
    stage(4);

    app_init();

    for (;;) {
        IWDG_KR = 0xAAAA;
        usb_poll();
        app_poll();
        if (usb_configured)
            stage(5);
    }
}
