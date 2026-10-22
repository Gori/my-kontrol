/* 128x32 SSD1306-class OLED, driven as in NI's firmware:
   SPI1 remapped to PB3 (SCK) / PB5 (MOSI), transmit-only, mode 3, 4.5 MHz.
   PB4 = D/C (0 command, 1 data), PB15 = CS (active low), PD2 = RESET (active low). */
#include "hw.h"
#include "display.h"

#define AFIO_MAPR  REG(0x40010004)
#define GPIOB_CRL  REG(0x40010C00)
#define GPIOB_CRH  REG(0x40010C04)
#define GPIOB_BSRR REG(0x40010C10)
#define GPIOB_BRR  REG(0x40010C14)
#define GPIOD_CRL  REG(0x40011400)
#define GPIOD_BSRR REG(0x40011410)
#define GPIOD_BRR  REG(0x40011414)
#define SPI1_CR1   REG(0x40013000)
#define SPI1_SR    REG(0x40013008)
#define SPI1_DR    REG(0x4001300C)

static void spi_send(uint8_t b)
{
    while (!(SPI1_SR & (1 << 1)))      /* TXE */
        ;
    SPI1_DR = b;
    while (!(SPI1_SR & (1 << 1)))
        ;
    while (SPI1_SR & (1 << 7))         /* BSY: finish before D/C changes */
        ;
}

static void cmd(uint8_t c)
{
    GPIOB_BRR = 1 << 4;
    spi_send(c);
}

void display_blit(const uint8_t *buf)
{
    for (int page = 0; page < 4; page++) {
        cmd(0xB0 | page);
        cmd(0x10);
        cmd(0x00);
        GPIOB_BSRR = 1 << 4;
        for (int x = 0; x < 128; x++)
            spi_send(buf[page * 128 + x]);
    }
}

void display_init(void)
{
    RCC_APB2ENR |= (1 << 0) | (1 << 3) | (1 << 5) | (1 << 12);  /* AFIO, GPIOB, GPIOD, SPI1 */
    /* SPI1 remap; JTAG off (frees PB3/PB4/PA15), SWD kept */
    AFIO_MAPR = (AFIO_MAPR & ~(7u << 24)) | (2u << 24) | 1;

    GPIOB_CRL = (GPIOB_CRL & ~0x00FFF000u) | 0x00B3B000;  /* PB3 AF, PB4 out, PB5 AF; 50 MHz */
    GPIOB_CRH = (GPIOB_CRH & ~0xF0000000u) | 0x30000000;  /* PB15 out */
    GPIOD_CRL = (GPIOD_CRL & ~0x00000F00u) | 0x00000300;  /* PD2 out */

    /* BIDIMODE|BIDIOE (tx only), SSM|SSI, /16, master, CPOL=1, CPHA=1 */
    SPI1_CR1 = 0xC31F;
    SPI1_CR1 |= 1 << 6;

    GPIOB_BRR = 1 << 15;
    GPIOD_BSRR = 1 << 2;
    delay_ms(2);
    GPIOD_BRR = 1 << 2;
    delay_ms(10);
    GPIOD_BSRR = 1 << 2;
    delay_ms(1);

    static const uint8_t init[] = {
        0xAE, 0x40, 0xB0, 0xC0, 0x81, 0xE0, 0xA0, 0xA6, 0xA8, 0x1F,
        0xD3, 0x00, 0xD5, 0xF0, 0xD9, 0x22, 0xDA, 0x02, 0xDB, 0x49,
    };
    for (unsigned i = 0; i < sizeof init; i++)
        cmd(init[i]);
}

void display_contrast(uint8_t v)
{
    cmd(0x81);
    cmd(v);
}

void display_on(void)
{
    cmd(0x8D);
    cmd(0x14);
    cmd(0xAF);
}
