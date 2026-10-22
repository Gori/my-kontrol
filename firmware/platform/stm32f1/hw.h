#pragma once
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* RCC */
#define RCC_CR       REG(0x40021000)
#define RCC_CFGR     REG(0x40021004)
#define RCC_APB2RSTR REG(0x4002100C)
#define RCC_APB1RSTR REG(0x40021010)
#define RCC_APB2ENR  REG(0x40021018)
#define RCC_APB1ENR  REG(0x4002101C)

/* Flash interface */
#define FLASH_ACR  REG(0x40022000)
#define FLASH_KEYR REG(0x40022004)
#define FLASH_SR   REG(0x4002200C)
#define FLASH_CR   REG(0x40022010)
#define FLASH_AR   REG(0x40022014)

/* GPIOA */
#define GPIOA_CRH  REG(0x40010804)
#define GPIOA_BSRR REG(0x40010810)
#define GPIOA_BRR  REG(0x40010814)

/* Independent watchdog key register */
#define IWDG_KR REG(0x40003000)

/* SCB / SysTick */
#define SCB_VTOR  REG(0xE000ED08)
#define SCB_AIRCR REG(0xE000ED0C)
#define SYST_CSR  REG(0xE000E010)
#define SYST_RVR  REG(0xE000E014)
#define SYST_CVR  REG(0xE000E018)

/* USB full-speed device */
#define USB_EPR(n)  REG(0x40005C00 + 4 * (n))
#define USB_CNTR    REG(0x40005C40)
#define USB_ISTR    REG(0x40005C44)
#define USB_DADDR   REG(0x40005C4C)
#define USB_BTABLE  REG(0x40005C50)
#define USB_PMA     0x40006000u

/* NI bootloader stays in DFU when this page starts with MAGIC (see notes). */
#define BOOT_FLAG_ADDR 0x0801FC00u

/* platform diagnostics (startup.c) */
int boot_flag_armed(void);
extern uint16_t prev_stage, prev_boots, crash_count;
extern uint32_t reset_flags;
