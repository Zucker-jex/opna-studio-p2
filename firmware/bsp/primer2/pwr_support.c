#include "pwr_support.h"
#include "primer2_pins.h"
#include "board.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define GPIOC_BASE 0x40011000u
#define GPIOA_BASE 0x40010800u
#define GPIO_CRL(b) REG32((b) + 0x00)
#define GPIO_CRH(b) REG32((b) + 0x04)
#define GPIO_IDR(b) REG32((b) + 0x08)
#define GPIO_BSRR(b) REG32((b) + 0x10)
#define RCC_APB2ENR REG32(0x40021018)

#define CNF_OUT_PP_50 0x3u

#define PWR_ON()     do { GPIO_BSRR(GPIOC_BASE) = (1u << (PRIMER2_SHDN_PIN + 16)); } while(0)
#define PWR_OFF()    do { GPIO_BSRR(GPIOC_BASE) = (1u << PRIMER2_SHDN_PIN); } while(0)

static void pin_cfg(uint32_t base, int pin, uint32_t cfg) {
    volatile uint32_t *r = (pin < 8) ? &GPIO_CRL(base) : &GPIO_CRH(base);
    int s = (pin & 7) * 4;
    *r = (*r & ~(0xFu << s)) | (cfg << s);
}

void pwr_init(void) {
    RCC_APB2ENR |= (1u << 4);
    pin_cfg(GPIOC_BASE, PRIMER2_SHDN_PIN, CNF_OUT_PP_50);
    PWR_ON();
}

void pwr_shutdown(void) {
    board_audio_mute(1);
    board_audio_close();

    board_lcd_backlight(0);

    PWR_OFF();

    for (;;) {
        __asm__ volatile("nop");
    }
}