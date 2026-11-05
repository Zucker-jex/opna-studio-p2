/*
 * Real STM32 Primer2 board backend.
 *
 * Register-level, no vendor SPL. The LCD path (FSMC bank NORSRAM3 @0x68000000,
 * GPIO chip-select, ST7732 controller, byte<<4 on the 16-bit bus) is taken from
 * the CircleOS 4.62 driver (Circle/OS/Hardware/Primer_1_2/lcd_spe.c), which is
 * the authoritative source for this panel.
 *
 * Clock: 12 MHz HSE -> PLL x6 -> 72 MHz.
 * Tracks live in flash (gen/songs.h); no SD driver yet. Audio plays through the
 * STW5094A codec (I2S3 + DMA2_Ch2), resampled from 55466 Hz to 48 kHz.
 */
#include "board.h"
#include "primer2_pins.h"
#include "pwr_support.h"
#include "pfm/pfm_config.h"
#include "pfm/pfm_prof.h"
#include "ff.h"
#include <stdint.h>

/* ---------------- registers ---------------- */
#define REG32(a) (*(volatile uint32_t *)(a))

#define RCC_CR       REG32(0x40021000)
#define RCC_CFGR     REG32(0x40021004)
#define RCC_APB2ENR  REG32(0x40021018)
#define RCC_AHBENR   REG32(0x40021014)
#define FLASH_ACR    REG32(0x40022000)

#define GPIOA_BASE 0x40010800u
#define GPIOB_BASE 0x40010C00u
#define GPIOC_BASE 0x40011000u
#define GPIOD_BASE 0x40011400u
#define GPIOE_BASE 0x40011800u
#define GPIO_CRL(b) REG32((b) + 0x00)
#define GPIO_CRH(b) REG32((b) + 0x04)
#define GPIO_IDR(b) REG32((b) + 0x08)
#define GPIO_ODR(b) REG32((b) + 0x0C)
#define GPIO_BSRR(b) REG32((b) + 0x10)

#define FSMC_BCR3 REG32(0xA0000010)
#define FSMC_BTR3 REG32(0xA0000014)

#define SYST_CSR  REG32(0xE000E010)
#define SYST_RVR  REG32(0xE000E014)
#define SYST_CVR  REG32(0xE000E018)

#define RCC_APB1ENR REG32(0x4002101C)
#define TIM4_BASE  0x40000800u
#define TIM4_CR1   REG32(TIM4_BASE + 0x00)
#define TIM4_EGR   REG32(TIM4_BASE + 0x14)
#define TIM4_CCMR2 REG32(TIM4_BASE + 0x1C)
#define TIM4_CCER  REG32(TIM4_BASE + 0x20)
#define TIM4_PSC   REG32(TIM4_BASE + 0x28)
#define TIM4_ARR   REG32(TIM4_BASE + 0x2C)
#define TIM4_CCR3  REG32(TIM4_BASE + 0x3C)

#define AFIO_MAPR  REG32(0x40010004)
#define I2C2_BASE  0x40005800u
#define I2C2_CR1   REG32(I2C2_BASE + 0x00)
#define I2C2_CR2   REG32(I2C2_BASE + 0x04)
#define I2C2_DR    REG32(I2C2_BASE + 0x10)
#define I2C2_SR1   REG32(I2C2_BASE + 0x14)
#define I2C2_SR2   REG32(I2C2_BASE + 0x18)
#define I2C2_CCR   REG32(I2C2_BASE + 0x1C)
#define I2C2_TRISE REG32(I2C2_BASE + 0x20)
#define SPI3_BASE  0x40003C00u
#define SPI3_CR2      REG32(SPI3_BASE + 0x04)
#define SPI3_I2SCFGR  REG32(SPI3_BASE + 0x1C)
#define SPI3_I2SPR    REG32(SPI3_BASE + 0x20)
/* LIS3LV02 accelerometer on SPI1: PA5=SCK PA6=MISO PA7=MOSI PE2=/CS */
#define SPI1_BASE  0x40013000u
#define SPI1_CR1   REG32(SPI1_BASE + 0x00)
#define SPI1_CR2   REG32(SPI1_BASE + 0x04)
#define SPI1_SR    REG32(SPI1_BASE + 0x08)
#define SPI1_DR    REG32(SPI1_BASE + 0x0C)
#define MEMS_CS_PIN 2 /* GPIOE */
#define DMA2_BASE  0x40020400u
#define DMA2_CCR2   REG32(DMA2_BASE + 0x1C)
#define DMA2_CNDTR2 REG32(DMA2_BASE + 0x20)
#define DMA2_CPAR2  REG32(DMA2_BASE + 0x24)
#define DMA2_CMAR2  REG32(DMA2_BASE + 0x28)

#define CNF_AF_OD_50 0xFu /* alternate function open-drain, 50 MHz */

/* GPIO 4-bit configs */
#define CNF_OUT_PP_50 0x3u  /* MODE=11 CNF=00 */
#define CNF_AF_PP_50  0xBu  /* MODE=11 CNF=10 */
#define CNF_IN_PULL   0x8u  /* MODE=00 CNF=10 (pull dir from ODR) */

static void pin_cfg(uint32_t base, int pin, uint32_t cfg) {
  volatile uint32_t *r = (pin < 8) ? &GPIO_CRL(base) : &GPIO_CRH(base);
  int s = (pin & 7) * 4;
  *r = (*r & ~(0xFu << s)) | (cfg << s);
}
static void pin_set(uint32_t base, int pin, int v) {
  GPIO_BSRR(base) = v ? (1u << pin) : (1u << (pin + 16));
}
static int pin_get(uint32_t base, int pin) { return (GPIO_IDR(base) >> pin) & 1u; }

/* ---------------- clock + delay ----------------
   Primer2 has a 12 MHz HSE crystal. The STM32F103's rated max is 72 MHz, but the
   FM render is CPU-bound, so we overclock the core (HCLK = SYSCLK) to 96 MHz for
   headroom. To keep peripherals within spec we divide the buses down: APB1 = /4
   = 24 MHz, APB2 = /2 = 48 MHz. Flash stays at 2 wait states + prefetch (out of
   spec above 72 MHz, but the prefetch buffer covers it at 96 MHz on this part).

   The I2S audio bit clock is derived from SYSCLK, so the codec sample rate moves
   with it; I2S_FS (below) is computed from SYSCLK_HZ and the resampler targets
   that exact value, so pitch stays correct at any of these clock choices. To
   change the overclock, edit PLL_MUL only (and re-check I2S_I2SDIV for ~48 kHz). */
#define HSE_HZ     12000000u
#define PLL_MUL    11u                       /* x11 -> 132 MHz: max stable overclock
   (144 MHz was unstable; the F103 core voltage is a fixed 1.8V LDO, not adjustable). */
#define SYSCLK_HZ  (HSE_HZ * PLL_MUL)       /* 132 MHz */
#define APB1_HZ    (SYSCLK_HZ / 4u)         /* 33 MHz (PPRE1 = /4) */

static void clock_init(void) {
  RCC_CR |= (1u << 16);                        /* HSEON */
  for (volatile int t = 0; t < 400000; t++)
    if (RCC_CR & (1u << 17)) break;            /* HSERDY */
  FLASH_ACR = 0x12;                            /* prefetch + 2 wait states */
  RCC_CR &= ~(1u << 24);                       /* PLL off while configuring */
  /* PLLSRC=HSE, PLLXTPRE=0, PLLMUL=(PLL_MUL-2), PPRE1=/4 (101), PPRE2=/2 (100) */
  RCC_CFGR = (1u << 16) | ((PLL_MUL - 2u) << 18) | (0x5u << 8) | (0x4u << 11) | (2u << 14); /* ADC /6 = 11 MHz */
  RCC_CR |= (1u << 24);                        /* PLLON */
  for (volatile int t = 0; t < 400000; t++)
    if (RCC_CR & (1u << 25)) break;            /* PLLRDY */
  RCC_CFGR |= 0x2u;                            /* SW = PLL */
  for (volatile int t = 0; t < 400000; t++)
    if ((RCC_CFGR & 0xCu) == 0x8u) break;      /* SWS = PLL */
}

/* Busy-wait via the DWT cycle counter (SysTick belongs to the FreeRTOS tick). */
static void delay_ms(uint32_t ms) {
  volatile uint32_t *cyccnt = (volatile uint32_t *)0xE0001004u; /* DWT_CYCCNT */
  while (ms--) {
    uint32_t start = *cyccnt;
    while ((*cyccnt - start) < (SYSCLK_HZ / 1000u)) {}
  }
}

/* ---------------- LCD (ST7732 over FSMC) ---------------- */
#define LCD_CMD_ADDR PRIMER2_LCD_CMD_ADDR
#define LCD_DAT_ADDR PRIMER2_LCD_DAT_ADDR
/* DB0..DB7 are wired to FSMC_D4..D11 -> every byte goes out as (b << 4). */
static inline void lcd_cmd(uint8_t c) { *(volatile uint16_t *)LCD_CMD_ADDR = (uint16_t)(c << 4); }
static inline void lcd_dat(uint8_t d) { *(volatile uint16_t *)LCD_DAT_ADDR = (uint16_t)(d << 4); }

static void fsmc_init(void) {
  RCC_AHBENR |= (1u << 8); /* FSMCEN */
  /* data: PE7..PE14 AF-PP */
  for (int p = 7; p <= 14; p++) pin_cfg(GPIOE_BASE, p, CNF_AF_PP_50);
  /* control: PD4=NOE, PD5=NWE, PD11=A16 (RS) AF-PP */
  pin_cfg(GPIOD_BASE, 4, CNF_AF_PP_50);
  pin_cfg(GPIOD_BASE, 5, CNF_AF_PP_50);
  pin_cfg(GPIOD_BASE, 11, CNF_AF_PP_50);
  /* RST(PD6) and CS(PD7) are plain GPIO outputs; CS stays low (always selected) */
  pin_cfg(GPIOD_BASE, PRIMER2_LCD_RST_PIN, CNF_OUT_PP_50);
  pin_cfg(GPIOD_BASE, 7, CNF_OUT_PP_50);
  pin_set(GPIOD_BASE, PRIMER2_LCD_RST_PIN, 1);
  pin_set(GPIOD_BASE, 7, 0);

  /* ADDSET=5 ADDHLD=5 DATAST=10 BUSTURN=15 CLKDIV=5 DATLAT=5 ACCMOD=A(0).
     The original CircleOS values (DATAST=2) were for the 72/96 MHz clock; at
     132 MHz that is a ~15 ns data phase, too fast for the ST7732 and a source
     of horizontal-line corruption.  ~60 ns here. */
  FSMC_BTR3 = 5u | (5u << 4) | (10u << 8) | (15u << 16) | (5u << 20) | (5u << 24);
  /* MBKEN | MWID=16b | WREN */
  FSMC_BCR3 = (1u << 0) | (1u << 4) | (1u << 12);
}

static void lcd_controller_init(void) {
  delay_ms(100);
  pin_set(GPIOD_BASE, PRIMER2_LCD_RST_PIN, 0);
  delay_ms(100);
  pin_set(GPIOD_BASE, PRIMER2_LCD_RST_PIN, 1);
  delay_ms(100);

  lcd_cmd(0x01); delay_ms(180);          /* SWRESET */
  lcd_cmd(0x10); delay_ms(100);          /* SLPIN   */
  lcd_cmd(0x11); delay_ms(150);          /* SLPOUT  */

  lcd_cmd(0xB1); lcd_dat(0x06); lcd_dat(0x03); lcd_dat(0x02); /* FRMCTR1 */
  lcd_cmd(0xB4); lcd_dat(0x03);                                /* INVCTR  */
  lcd_cmd(0xB6); lcd_dat(0x02); lcd_dat(0x0e);                 /* DISSET5 */
  lcd_cmd(0xF5); lcd_dat(0x1a);                                /* DISPCTRL*/
  lcd_cmd(0xC0); lcd_dat(0x02); lcd_dat(0x00);                 /* PWCTR1  */
  lcd_cmd(0xC1); lcd_dat(0x05);                                /* PWCTR2  */
  lcd_cmd(0xC2); lcd_dat(0x02); lcd_dat(0x02);                 /* PWCTR3  */
  lcd_cmd(0xC3); lcd_dat(0x01); lcd_dat(0x02);                 /* PWCTR4  */
  lcd_cmd(0xC4); lcd_dat(0x01); lcd_dat(0x02);                 /* PWCTR5  */
  lcd_cmd(0xC5); lcd_dat(0x47); lcd_dat(0x3a);                 /* VMCTR   */
  lcd_cmd(0xF2); lcd_dat(0x02);                                /* OSCADJ  */
  lcd_cmd(0xF6); lcd_dat(0x4c);                                /* DEFADJ  */
  lcd_cmd(0xF8); lcd_dat(0x06);                                /* LOAD    */

  static const uint8_t g1[] = {0x06,0x1c,0x1f,0x1f,0x18,0x13,0x06,0x03,0x03,0x04,0x07,0x07,0x00};
  static const uint8_t g2[] = {0x0a,0x14,0x1b,0x18,0x12,0x0e,0x02,0x01,0x00,0x01,0x08,0x07,0x00};
  lcd_cmd(0xE0); for (unsigned i = 0; i < sizeof(g1); i++) lcd_dat(g1[i]);
  lcd_cmd(0xE1); for (unsigned i = 0; i < sizeof(g2); i++) lcd_dat(g2[i]);

  /* MADCTL: portrait 128x160, rotated 180 deg (MX+MY) so the panel is upright
     for the Primer2's physical orientation. RGB order. */
  lcd_cmd(0x36); lcd_dat(0xC0);
  lcd_cmd(0x3A); lcd_dat(0x55);          /* COLMOD: 16 bpp */
  lcd_cmd(0x35); lcd_dat(0x00);          /* TEON */
  lcd_cmd(0x29); delay_ms(20);           /* DISPON */
}

static void lcd_window(int x, int y, int w, int h) {
  int x1 = x + w - 1, y1 = y + h - 1;
  lcd_cmd(0x2A); lcd_dat(0); lcd_dat((uint8_t)x); lcd_dat(0); lcd_dat((uint8_t)x1);
  lcd_cmd(0x2B); lcd_dat(0); lcd_dat((uint8_t)y); lcd_dat(0); lcd_dat((uint8_t)y1);
  lcd_cmd(0x2C); /* RAMWR */
}
static inline void lcd_px(uint16_t c) { lcd_dat((uint8_t)(c >> 8)); lcd_dat((uint8_t)c); }

#define LCD_CLIP_MAX 8
static int16_t clip_r[LCD_CLIP_MAX][4];
static int clip_n;

static void lcd_fill_raw(int x, int y, int w, int h, uint16_t color);
static void lcd_blit_raw(int x, int y, int w, int h, const uint16_t *pix, int stride);

static uint16_t *cv_buf;
static int cv_on, cv_x, cv_y, cv_w, cv_h;

void board_lcd_canvas_begin(uint16_t *buf, int x, int y, int w, int h) {
  cv_buf = buf; cv_x = x; cv_y = y; cv_w = w; cv_h = h; cv_on = 1;
}
void board_lcd_canvas_end(void) {
  if (!cv_on) return;
  cv_on = 0;
  lcd_blit_raw(cv_x, cv_y, cv_w, cv_h, cv_buf, cv_w);
}
static int cv_rect(int x, int y, int w, int h, int *x0, int *y0, int *x1, int *y1) {
  *x0 = x > cv_x ? x : cv_x;
  *y0 = y > cv_y ? y : cv_y;
  *x1 = x + w < cv_x + cv_w ? x + w : cv_x + cv_w;
  *y1 = y + h < cv_y + cv_h ? y + h : cv_y + cv_h;
  return *x1 > *x0 && *y1 > *y0;
}

void board_lcd_clip_reset(void) { clip_n = 0; }
void board_lcd_clip_add(int x, int y, int w, int h) {
  if (clip_n >= LCD_CLIP_MAX || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > BOARD_LCD_W) w = BOARD_LCD_W - x;
  if (y + h > BOARD_LCD_H) h = BOARD_LCD_H - y;
  if (w <= 0 || h <= 0) return;
  clip_r[clip_n][0] = (int16_t)x; clip_r[clip_n][1] = (int16_t)y;
  clip_r[clip_n][2] = (int16_t)w; clip_r[clip_n][3] = (int16_t)h;
  clip_n++;
}

static void lcd_fill_raw(int x, int y, int w, int h, uint16_t color) {
  lcd_window(x, y, w, h);
  for (int i = w * h; i > 0; i--) lcd_px(color);
}
static void lcd_blit_raw(int x, int y, int w, int h, const uint16_t *pix, int stride) {
  lcd_window(x, y, w, h);
  for (int row = 0; row < h; row++) {
    const uint16_t *s = pix + row * stride;
    for (int k = 0; k < w; k++) lcd_px(*s++);
  }
}
static int clip_inside(int x, int y) {
  for (int i = 0; i < clip_n; i++)
    if (x >= clip_r[i][0] && x < clip_r[i][0] + clip_r[i][2] &&
        y >= clip_r[i][1] && y < clip_r[i][1] + clip_r[i][3]) return 1;
  return 0;
}

void board_lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  if (cv_on) {
    int x0, y0, x1, y1;
    if (cv_rect(x, y, w, h, &x0, &y0, &x1, &y1)) {
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
          cv_buf[(yy - cv_y) * cv_w + (xx - cv_x)] = color;
      return;
    }
  }
  if (clip_n == 0) { lcd_fill_raw(x, y, w, h, color); return; }
  for (int i = 0; i < clip_n; i++) {
    int x0 = x > clip_r[i][0] ? x : clip_r[i][0];
    int y0 = y > clip_r[i][1] ? y : clip_r[i][1];
    int x1 = x + w < clip_r[i][0] + clip_r[i][2] ? x + w : clip_r[i][0] + clip_r[i][2];
    int y1 = y + h < clip_r[i][1] + clip_r[i][3] ? y + h : clip_r[i][1] + clip_r[i][3];
    if (x1 > x0 && y1 > y0) lcd_fill_raw(x0, y0, x1 - x0, y1 - y0, color);
  }
}
void board_lcd_clear(uint16_t color) {
  board_lcd_clip_reset();
  lcd_fill_raw(0, 0, BOARD_LCD_W, BOARD_LCD_H, color);
}
void board_lcd_pixel(int x, int y, uint16_t color) {
  if ((unsigned)x >= BOARD_LCD_W || (unsigned)y >= BOARD_LCD_H) return;
  if (cv_on && x >= cv_x && x < cv_x + cv_w && y >= cv_y && y < cv_y + cv_h) {
    cv_buf[(y - cv_y) * cv_w + (x - cv_x)] = color;
    return;
  }
  if (clip_n && !clip_inside(x, y)) return;
  lcd_window(x, y, 1, 1);
  lcd_px(color);
}

/* One window for a whole glyph/bitmap: the single biggest UI speed-up. */
void board_lcd_blit(int x, int y, int w, int h, const uint16_t *pix) {
  if (w <= 0 || h <= 0) return;
  if (cv_on) {
    int x0, y0, x1, y1;
    if (cv_rect(x, y, w, h, &x0, &y0, &x1, &y1)) {
      for (int yy = y0; yy < y1; yy++) {
        const uint16_t *src = pix + (yy - y) * w + (x0 - x);
        uint16_t *dst = cv_buf + (yy - cv_y) * cv_w + (x0 - cv_x);
        for (int xx = x0; xx < x1; xx++) *dst++ = *src++;
      }
      return;
    }
  }
  if (clip_n == 0) { lcd_blit_raw(x, y, w, h, pix, w); return; }
  for (int i = 0; i < clip_n; i++) {
    int x0 = x > clip_r[i][0] ? x : clip_r[i][0];
    int y0 = y > clip_r[i][1] ? y : clip_r[i][1];
    int x1 = x + w < clip_r[i][0] + clip_r[i][2] ? x + w : clip_r[i][0] + clip_r[i][2];
    int y1 = y + h < clip_r[i][1] + clip_r[i][3] ? y + h : clip_r[i][1] + clip_r[i][3];
    if (x1 > x0 && y1 > y0)
      lcd_blit_raw(x0, y0, x1 - x0, y1 - y0,
                   pix + (y0 - y) * w + (x0 - x), w);
  }
}
void board_lcd_present(void) { /* drawing goes straight to GRAM */ }

/* Backlight: TIM4_CH3 PWM on PB8, exactly as CircleOS (LCD_BackLightConfig).
   The panel enable is active-low but a transistor inverts it, so the MCU pin is
   effectively active-high via PWM1/polarity-high. */
static void backlight_init(void) {
  pin_cfg(GPIOB_BASE, PRIMER2_BL_PIN, CNF_AF_PP_50); /* PB8 -> TIM4_CH3 AF */
  RCC_APB1ENR |= (1u << 2);                          /* TIM4 clock */
  TIM4_PSC = 0;
  /* 132 MHz APB1 timer (66 MHz) / 4096 = 16.1 kHz: above the visible
     scan/flicker band (the old 0xFFFF -> ~1 kHz PWM could beat with the
     panel scan and look like horizontal lines). */
  TIM4_ARR = 0x0FFF;
  TIM4_CCR3 = 0x0FFF;
  TIM4_CCMR2 = (6u << 4);     /* OC3M = PWM mode 1 */
  TIM4_CCER = (1u << 8);      /* CC3E enable, CC3P = 0 (active high) */
  TIM4_EGR = 1;              /* load registers (UG) */
  TIM4_CR1 = 1;              /* enable counter (CEN) */
}

/* Backlight brightness 0..BOARD_BL_MAX -> TIM4_CH3 PWM duty. Roughly perceptual
   (exponential-ish) so the low steps aren't all indistinguishably bright. */
void board_lcd_backlight(int level) {
  static const uint16_t duty[BOARD_BL_MAX + 1] = {
    0x0040, 0x00C0, 0x0180, 0x0300, 0x0580, 0x0880, 0x0C00, 0x0FFF
  };
  if (level < 0) level = 0;
  if (level > BOARD_BL_MAX) level = BOARD_BL_MAX;
  TIM4_CCR3 = duty[level];
}

/* ---------------- joystick ---------------- */
/* Pressed shorts the line to the pulled-up common => reads HIGH. Inputs use
   internal pull-downs so an open contact reads LOW. */
static int joy_raw(void) {
  int m = 0;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_UP_PIN)) m |= BTN_UP;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_DOWN_PIN)) m |= BTN_DOWN;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_LEFT_PIN)) m |= BTN_LEFT;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_RIGHT_PIN)) m |= BTN_RIGHT;
  if (pin_get(GPIOA_BASE, PRIMER2_JOY_CENTER_PIN)) m |= BTN_CENTER;
  return m;
}

int board_input_poll(void) { return joy_raw(); }

/* ---------------- LIS3LV02 3-axis accelerometer (SPI1) ----------------
   The Primer2 has a LIS3LV02 behind PE2 /CS on SPI1.  Used for the tilt
   performance control.  If the chip is absent the UI shows "N/A" and the
   engine simply keeps the last bend. */
static int g_mems_ok;

static uint8_t spi1_xfer(uint8_t v) {
  while (!(SPI1_SR & (1u << 1))) {}     /* TXE */
  SPI1_DR = v;
  while (!(SPI1_SR & (1u << 0))) {}     /* RXNE */
  return (uint8_t)SPI1_DR;
}
static uint8_t mems_readreg(uint8_t reg) {
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 0);
  spi1_xfer((uint8_t)(reg | 0x80));
  uint8_t v = spi1_xfer(0);
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 1);
  return v;
}
static void mems_writereg(uint8_t reg, uint8_t val) {
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 0);
  spi1_xfer((uint8_t)(reg & 0x7f));
  spi1_xfer(val);
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 1);
}
static void mems_init(void) {
  RCC_APB2ENR |= (1u << 12);            /* SPI1 */
  pin_cfg(GPIOA_BASE, 5, CNF_AF_PP_50);  /* SCK */
  pin_cfg(GPIOA_BASE, 6, CNF_AF_PP_50);  /* MISO */
  pin_cfg(GPIOA_BASE, 7, CNF_AF_PP_50);  /* MOSI */
  pin_cfg(GPIOE_BASE, MEMS_CS_PIN, CNF_OUT_PP_50);
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 1);
  SPI1_CR2 = 0;
  /* MSTR, BR=/64 (~1 MHz at 66 MHz APB2), SPE, SSM+SSI (GPIO chip select) */
  SPI1_CR1 = (1u << 2) | (5u << 3) | (1u << 6) | (1u << 8) | (1u << 9);
  for (volatile int i = 0; i < 1000; i++) {}
  g_mems_ok = 0;
  if (mems_readreg(0x0F) != 0x3A) return;
  mems_writereg(0x20, 0xC7);  /* 200 Hz, all axes, normal mode */
  mems_writereg(0x21, 0x00);
  mems_writereg(0x22, 0x00);
  g_mems_ok = 1;
}

int board_motion(int16_t *x, int16_t *y, int16_t *z) {
  if (!g_mems_ok) return -1;
  uint8_t b[6];
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 0);
  spi1_xfer(0x28 | 0x80 | 0x40);       /* read + auto-increment */
  for (int i = 0; i < 6; i++) b[i] = spi1_xfer(0);
  pin_set(GPIOE_BASE, MEMS_CS_PIN, 1);
  int16_t raw[3];
  for (int i = 0; i < 3; i++)
    raw[i] = (int16_t)((uint16_t)b[i * 2] | ((uint16_t)b[i * 2 + 1] << 8));
  for (int i = 0; i < 3; i++) {
    int v = (raw[i] >> 4) / 2;         /* 12-bit signed -> ~+/-1024 */
    if (v < -1000) v = -1000;
    if (v > 1000) v = 1000;
    raw[i] = (int16_t)v;
  }
  if (x) *x = raw[0];
  if (y) *y = raw[1];
  if (z) *z = raw[2];
  return 0;
}

/* DWT cycle counter (Cortex-M3 core debug) for the render perf meter. */
#define DEMCR     REG32(0xE000EDFC)
#define DWT_CTRL  REG32(0xE0001000)
#define DWT_CYCCNT REG32(0xE0001004)
uint32_t board_cycles(void) { return DWT_CYCCNT; }
uint32_t board_cpu_hz(void) { return SYSCLK_HZ; }

int board_input_wait(void) {
  static int prev;
  for (;;) {
    int cur = joy_raw();
    int edge = cur & ~prev;
    prev = cur;
    if (edge) {
      delay_ms(30); /* debounce */
      return edge;
    }
    delay_ms(10);
    /* heartbeat so we can see the firmware is alive */
    static uint32_t t;
    if (((++t) & 0x3f) == 0) GPIO_ODR(GPIOE_BASE) ^= (1u << PRIMER2_LED0_PIN);
  }
}

/* ---------------- Audio: STW5094A codec via I2S3 (SPI3) + DMA2_Ch2 ----------
   The circular DMA buffer IS the FIFO between the emulator and the codec:
     - producer: pfm_player_render() -> board_audio_write() pushes OPNA frames
       1:1 (no resample); ring_push() stalls (busy-waits) on backpressure when
       the FIFO is full, so the OPNA runs as fast as it can but no faster.
     - consumer: DMA2 pops the FIFO at a rock-steady rate set purely by the I2S
       bit clock (I2S_FS), feeding the codec at exactly that rate.
   We therefore program the I2S divider so I2S_FS matches the OPNA's native rate
   (PFM_MIX_RATE = 55467 Hz) as closely as the divider allows -> no resampling.
   Output is routed to the headphone jack only (CR6 bits 0x0c = PHL/PHR); the
   loudspeaker (bit 0x10) is off, as this board has no HP-detect line. */

#define AUD_RING 1024                    /* stereo frames in the DMA FIFO (~18 ms) */
static int16_t g_ring[AUD_RING * 2];     /* interleaved L,R @ I2S_FS */
static unsigned g_wr;                    /* write cursor (in samples) */

/* I2S Fs = SYSCLK / (32 * (2*I2SDIV + ODD)). Pick the divider closest to the
   OPNA rate: at 96 MHz, I2SDIV=27, ODD=0 -> 96e6/(32*54) = 55556 Hz, i.e.
   PFM_MIX_RATE + 0.16% (~3 cents, inaudible). So the codec consumes exactly
   what the emulator produces, 1:1. */
#define I2S_I2SDIV 37u
#define I2S_ODD    0u   /* 2*37+0 = 74 -> 132e6/(32*74) = 55743 Hz (~OPNA rate, +0.5%) */
#define I2S_FS (SYSCLK_HZ / (32u * (2u * I2S_I2SDIV + I2S_ODD))) /* 55743 Hz */

static void i2c_codec_write(const uint8_t *cr, int n) {
  volatile uint32_t t;
  I2C2_CR1 |= (1u << 8);                              /* START */
  t = 200000; while (!(I2C2_SR1 & (1u << 0)) && --t) {} /* SB */
  I2C2_DR = 0xE2;                                     /* addr, write */
  t = 200000; while (!(I2C2_SR1 & (1u << 1)) && --t) {} /* ADDR */
  (void)I2C2_SR2;                                     /* clear ADDR */
  I2C2_DR = 0x00;                                     /* sub-address (reg 0) */
  t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {} /* TXE */
  for (int i = 0; i < n; i++) {
    I2C2_DR = cr[i];
    t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {} /* TXE */
  }
  t = 200000; while (!(I2C2_SR1 & (1u << 2)) && --t) {} /* BTF */
  I2C2_CR1 |= (1u << 9);                              /* STOP */
}

/* Write a single codec register (sub-address = reg). */
static void i2c_codec_write_reg(uint8_t reg, uint8_t val) {
  volatile uint32_t t;
  I2C2_CR1 |= (1u << 8);
  t = 200000; while (!(I2C2_SR1 & (1u << 0)) && --t) {}
  I2C2_DR = 0xE2;
  t = 200000; while (!(I2C2_SR1 & (1u << 1)) && --t) {}
  (void)I2C2_SR2;
  I2C2_DR = reg;
  t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {}
  I2C2_DR = val;
  t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {}
  t = 200000; while (!(I2C2_SR1 & (1u << 2)) && --t) {}
  I2C2_CR1 |= (1u << 9);
}

/* Digital volume gain (Q15), applied to samples in board_audio_write so it works
   for headphone AND loudspeaker. Roughly perceptual (each step ~ a few dB). */
static int32_t g_vol_q15 = 8192;
static const int32_t vol_q15_tab[BOARD_VOL_MAX + 1] = {
  0, 512, 1024, 2048, 4096, 8192, 12288, 17408, 23552, 28672, 32767,
};

/* Volume 0..BOARD_VOL_MAX -> digital gain (both outputs). */
void board_audio_set_volume(int level) {
  if (level < 0) level = 0;
  if (level > BOARD_VOL_MAX) level = BOARD_VOL_MAX;
  /* Digital gain applied in board_audio_write, so it works for BOTH the headphone
     and the loudspeaker path (the codec HP-gain regs only affect headphones). The
     codec analog gains stay at their loud default; this scales the samples. */
  g_vol_q15 = vol_q15_tab[level];
}

/* CR6 routing: bit5 = MUT, 0x10 = loudspeaker, 0x0c = headphone, 0x02 = SE.
   Kept in a shadow so mute and output-select compose without clobbering. */
static uint8_t g_cr6 = 0x0c | 0x02;

void board_audio_mute(int on) {
  if (on) g_cr6 |= 0x20u;
  else    g_cr6 &= ~0x20u;
  i2c_codec_write_reg(6, g_cr6);
}

void board_audio_set_output(int speaker) {
  g_cr6 = (uint8_t)((speaker ? 0x10u : 0x0cu) | 0x02u | (g_cr6 & 0x20u));
  i2c_codec_write_reg(6, g_cr6);
}

static void codec_init(void) {
  RCC_APB2ENR |= (1u << 0);   /* AFIO */
  RCC_APB1ENR |= (1u << 22);  /* I2C2 */
  pin_cfg(GPIOB_BASE, 10, CNF_AF_OD_50); /* SCL */
  pin_cfg(GPIOB_BASE, 11, CNF_AF_OD_50); /* SDA */
  I2C2_CR1 = 0;
  I2C2_CR2 = APB1_HZ / 1000000u;             /* APB1 clock in MHz (=24) */
  I2C2_CCR = APB1_HZ / (2u * 100000u);       /* 100 kHz standard mode (=120) */
  I2C2_TRISE = APB1_HZ / 1000000u + 1u;      /* (=25) */
  I2C2_CR1 = 1;              /* enable */

  uint8_t cr[22] = {0};
  cr[0] = 0x00;
  cr[1] = 0x14;
  cr[4] = 0x6f;
  cr[5] = 0x17;
  cr[6] = g_cr6;              /* headphone (PHL/PHR) + SE, loudspeaker off, not muted */
  cr[7] = 0x00;               /* CR7 LSA(3:0)=0000 -> earpiece/loudspeaker amp at its
                                 max +6 dB gain (was 0x04 = -2 dB, i.e. 8 dB quieter).
                                 Fine level is still the digital volume; drop a step or
                                 two if loud tracks clip the 8 ohm / 300 mW driver. */
  cr[8] = 0x00;               /* HP gain L: loudest (digital volume scales samples) */
  cr[9] = 0x00;               /* HP gain R: loudest */
  cr[12] = 0x84;
  cr[13] = 89;
  cr[14] = 89;
  cr[16] = 0x08;
  cr[18] = 0x60;
  cr[21] = 0x61;              /* audio mode + power on */
  i2c_codec_write(cr, 22);
}

static void i2s_dma_init(void) {
  /* disable JTAG, keep SWD -> frees PB3 (I2S3_CK) and PA15 (I2S3_WS) */
  AFIO_MAPR = (AFIO_MAPR & ~(7u << 24)) | (2u << 24); /* SWJ_CFG = 010 */
  RCC_APB1ENR |= (1u << 15);  /* SPI3/I2S3 */
  RCC_AHBENR |= (1u << 1);    /* DMA2 */
  pin_cfg(GPIOB_BASE, 3, CNF_AF_PP_50);  /* I2S3_CK */
  pin_cfg(GPIOB_BASE, 5, CNF_AF_PP_50);  /* I2S3_SD */
  pin_cfg(GPIOA_BASE, 15, CNF_AF_PP_50); /* I2S3_WS */

  SPI3_I2SCFGR = 0;
  SPI3_I2SPR = I2S_I2SDIV | (I2S_ODD << 8); /* -> I2S_FS = OPNA rate */
  /* I2SMOD | cfg=master-tx (10) | std=MSB/left-justified (01) | 16-bit */
  SPI3_I2SCFGR = (1u << 11) | (2u << 8) | (1u << 4);
  SPI3_CR2 = (1u << 1);                  /* TXDMAEN */

  DMA2_CCR2 = 0;
  DMA2_CPAR2 = SPI3_BASE + 0x0C;         /* &SPI3->DR */
  DMA2_CMAR2 = (uint32_t)g_ring;
  DMA2_CNDTR2 = AUD_RING * 2;
  /* DIR mem->periph, CIRC, MINC, PSIZE=16, MSIZE=16, PL=high */
  DMA2_CCR2 = (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 10) | (2u << 12);
  DMA2_CCR2 |= 1u;                       /* enable */
  SPI3_I2SCFGR |= (1u << 10);            /* I2SE */
}

static unsigned ring_read_pos(void) { return (AUD_RING * 2) - DMA2_CNDTR2; }

/* Drop metric: the DMA read cursor advances at exactly I2S_FS (it is our clock).
   Count cumulative samples it consumes vs samples the producer has written; any
   time consumption out-runs production the ring underran and those frames were
   stale = dropped. */
static uint32_t g_written_s;   /* cumulative samples written (ring pre-fill incl.) */
static uint32_t g_consumed_s;  /* cumulative samples the DMA has read */
static uint32_t g_dropped_s;   /* cumulative stale (dropped) samples */
static unsigned g_last_rd;     /* previous DMA read position */
uint32_t board_audio_underruns(void) { return g_dropped_s >> 1; } /* frames */
uint32_t board_audio_consumed_frames(void) { return g_consumed_s >> 1; }

/* Instantaneous ring headroom in frames (signed; <0 means underrunning). Used
   by the UI to run the LCD at lowest priority: it only paints when there is
   enough buffered audio to survive the draw, so it never starves the codec. */
int32_t board_audio_ring_fill(void) {
  unsigned rd = ring_read_pos() & ~1u;
  uint32_t consumed_now = g_consumed_s + ((rd - g_last_rd) & (AUD_RING * 2 - 1u));
  return (int32_t)(g_written_s - consumed_now) >> 1;
}

void board_audio_open(unsigned rate, uint32_t total_frames) {
  (void)rate; (void)total_frames;
  for (unsigned i = 0; i < AUD_RING * 2; i++) g_ring[i] = 0;
  g_wr = 0;
  g_written_s = AUD_RING * 2; /* ring starts pre-filled with silence */
  g_consumed_s = 0;
  g_dropped_s = 0;
  g_last_rd = 0;
  codec_init();
  i2s_dma_init();
}

/* Push OPNA frames into the DMA FIFO 1:1 (no resample). Copies in contiguous
   runs, reading the DMA position only once per run instead of per frame. In
   SRAM (.ramfunc): the copy loop must not run from the under-spec flash. */
PFM_HOT void board_audio_write(const int16_t *src, size_t frames) {
  uint32_t prof_t0 = pfm_prof_begin();
  const unsigned size = AUD_RING * 2;         /* samples in the ring */
  /* advance the consumed counter and account any underrun since the last call */
  {
    unsigned rd = ring_read_pos() & ~1u;
    g_consumed_s += (rd - g_last_rd) & (size - 1u);
    g_last_rd = rd;
    int32_t deficit = (int32_t)(g_consumed_s - g_written_s);
    if (deficit > 0) { g_dropped_s += (uint32_t)deficit; g_written_s = g_consumed_s; }
  }
  g_written_s += (uint32_t)(frames * 2);
  size_t total = frames * 2, done = 0;
  while (done < total) {
    unsigned rd = ring_read_pos() & ~1u;      /* DMA read cursor (one AHB read) */
    unsigned wr = g_wr;
    unsigned freesmp = (rd - wr - 2u) & (size - 1u); /* free samples, 1-frame gap */
    if (freesmp == 0) { __asm__ volatile("nop"); continue; } /* full: wait for DMA */
    unsigned run = size - wr;                 /* contiguous run to ring end */
    if (run > freesmp) run = freesmp;
    unsigned rem = (unsigned)(total - done);
    if (run > rem) run = rem;
    int32_t vol = g_vol_q15;
    for (unsigned k = 0; k < run; k++)
      g_ring[wr + k] = (int16_t)(((int32_t)src[done + k] * vol) >> 15);
    wr += run;
    if (wr >= size) wr -= size;
    g_wr = wr;
    done += run;
  }
  pfm_prof_end(PFM_PROF_OUTPUT, prof_t0);
}

void board_audio_close(void) {
  /* let the ring drain, then stop and mute */
  for (int guard = 0; guard < 200000; guard++) {
    unsigned rd = ring_read_pos() & ~1u;
    if (rd == g_wr) break;
    __asm__ volatile("nop");
  }
  SPI3_I2SCFGR &= ~(1u << 10); /* I2SE off */
  DMA2_CCR2 = 0;
  uint8_t mute[22] = {0};
  mute[6] = 0x20; /* MUT */
  i2c_codec_write(mute, 22);
}

/* Re-prime the ring + DMA to a clean pre-filled state WITHOUT touching the codec.
   Called on song switch: the blocking SD read stalls the producer past one ring
   period, so the DMA read pointer wraps more than once and the consumed counter
   under-counts permanently (ring_fill drifts high -> renderer stops). Resetting
   the counters + DMA here clears that drift so every switch starts fresh. */
void board_audio_restart(void) {
  SPI3_I2SCFGR &= ~(1u << 10);            /* I2SE off */
  DMA2_CCR2 &= ~1u;                        /* DMA disable */
  for (unsigned i = 0; i < AUD_RING * 2; i++) g_ring[i] = 0;
  g_wr = 0;
  g_written_s = AUD_RING * 2;              /* pre-filled with silence */
  g_consumed_s = 0;
  g_dropped_s = 0;
  g_last_rd = 0;
  DMA2_CNDTR2 = AUD_RING * 2;
  DMA2_CMAR2 = (uint32_t)g_ring;
  DMA2_CCR2 |= 1u;                         /* DMA enable */
  SPI3_I2SCFGR |= (1u << 10);             /* I2SE on */
}

/* ---------------- storage: SD only (no flash-embedded songs) ---------------- */
int board_storage_count(void) { return 0; }
const char *board_storage_name(int i) { (void)i; return ""; }
const char *board_storage_group(int i) { (void)i; return ""; }
int board_storage_load(int i, uint8_t *buf, size_t maxlen) {
  (void)i; (void)buf; (void)maxlen; return -1;
}

/* ---------------- files on the microSD card (FatFs) ---------------- */
int board_file_save(const char *name, const void *buf, int len) {
  FIL fp;
  UINT bw = 0;
  if (f_open(&fp, name, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
  FRESULT fr = f_write(&fp, buf, (UINT)len, &bw);
  f_close(&fp);
  return (fr == FR_OK && bw == (UINT)len) ? 0 : -1;
}

int board_file_load(const char *name, void *buf, int maxlen) {
  FIL fp;
  UINT br = 0;
  if (f_open(&fp, name, FA_READ) != FR_OK) return -1;
  FRESULT fr = f_read(&fp, buf, (UINT)maxlen, &br);
  f_close(&fp);
  return (fr == FR_OK) ? (int)br : -1;
}

/* ---------------- lifecycle ---------------- */
void board_log(const char *s) { (void)s; }

/* ---------------- Touch: 4-wire resistive on ADC3 (PC0..PC3) ---------------
   ADC mapping follows Chopstx's example-primer2/primer2-ts.c:
     injected group ch10=L, ch11=sample-only, ch12=U, ch13=D
     pressed when U < 0xc00
     x = 128*(1000*L/(4096-U) - 0x20)/0x100
     y = 160*(0x1e0 - 1000*(U-D)/(4096-U))/0x1c0
   The joystick/C logic is untouched; ui_touch() only moves selections.

   Three-point calibration uses the affine solve from CircleOS
   TOUCHSCR_CalculateCalibration() (Keolabs), itself based on the public-domain
   3-point calibration sample by Carlos E. Vidales.  The six raw points are
   kept in BKP_DR2..7 so the matrix can be rebuilt after reset. */
#define ADC3_BASE   0x40013C00u
#define ADC3_SR     REG32(ADC3_BASE + 0x00)
#define ADC3_CR1    REG32(ADC3_BASE + 0x04)
#define ADC3_CR2    REG32(ADC3_BASE + 0x08)
#define ADC3_SMPR1  REG32(ADC3_BASE + 0x0C)
#define ADC3_SMPR2  REG32(ADC3_BASE + 0x10)
#define ADC3_JSQR   REG32(ADC3_BASE + 0x38)
#define ADC3_JDR1   REG32(ADC3_BASE + 0x3C)
#define ADC3_JDR2   REG32(ADC3_BASE + 0x40)
#define ADC3_JDR3   REG32(ADC3_BASE + 0x44)
#define ADC3_JDR4   REG32(ADC3_BASE + 0x48)

static int cal_load_raw(int raw[6]);
static int cal_compute(const int raw[6]);

void board_touch_init(void) {
  RCC_APB2ENR |= 1u << 15;                 /* ADC3 */
  for (int p = 0; p < 4; p++) pin_cfg(GPIOC_BASE, p, 0x0); /* analog */
  ADC3_CR1 = 0;
  ADC3_CR2 = 1u;
  ADC3_CR2 = 1u | (1u << 3);               /* RSTCAL */
  while (ADC3_CR2 & (1u << 3)) { }
  ADC3_CR2 = 1u | (1u << 2);               /* CAL */
  while (ADC3_CR2 & (1u << 2)) { }
  ADC3_CR2 = 0;
  ADC3_CR1 = (1u << 8);                    /* SCAN */
  ADC3_SMPR1 = 0xFFF;                      /* ch10..13, 239.5 cycles */
  ADC3_SMPR2 = 0;
  ADC3_JSQR = ((4u - 1u) << 20) | (10u << 0) | (11u << 5) | (12u << 10) | (13u << 15);
  ADC3_CR2 = 1u | (1u << 15) | (7u << 12); /* ADON, JEXTTRIG, JSWSTART */
  int raw[6];
  if (cal_load_raw(raw)) cal_compute(raw);
}

#define BKP_BASE 0x40006C00u
#define BKP_DR1 REG32(BKP_BASE + 0x04)
#define BKP_DR2 REG32(BKP_BASE + 0x08)
#define BKP_DR3 REG32(BKP_BASE + 0x0C)
#define BKP_DR4 REG32(BKP_BASE + 0x10)
#define BKP_DR5 REG32(BKP_BASE + 0x14)
#define BKP_DR6 REG32(BKP_BASE + 0x18)
#define BKP_DR7 REG32(BKP_BASE + 0x1C)
#define PWR_CR  REG32(0x40007000)

/* Official CircleOS Primer2 reference points (touchscreen_spe.c). */
static const int cal_ref[3][2] = { {115, 15}, {115, 115}, {15, 15} };
static int cal_valid, cal_a, cal_b, cal_c, cal_d, cal_e, cal_f, cal_r;

static void bkp_enable(void) {
  RCC_APB1ENR |= (1u << 27) | (1u << 28); /* BKP + PWR */
  PWR_CR |= 1u << 8;                      /* DBP */
}

static void cal_save_raw(const int raw[6]) {
  bkp_enable();
  BKP_DR1 = 0x5A5A;
  BKP_DR2 = (uint16_t)raw[0]; BKP_DR3 = (uint16_t)raw[1];
  BKP_DR4 = (uint16_t)raw[2]; BKP_DR5 = (uint16_t)raw[3];
  BKP_DR6 = (uint16_t)raw[4]; BKP_DR7 = (uint16_t)raw[5];
}

static int cal_load_raw(int raw[6]) {
  bkp_enable();
  if ((BKP_DR1 & 0xFFFFu) != 0x5A5A) return 0;
  raw[0] = (int)(BKP_DR2 & 0xFFFFu); raw[1] = (int)(BKP_DR3 & 0xFFFFu);
  raw[2] = (int)(BKP_DR4 & 0xFFFFu); raw[3] = (int)(BKP_DR5 & 0xFFFFu);
  raw[4] = (int)(BKP_DR6 & 0xFFFFu); raw[5] = (int)(BKP_DR7 & 0xFFFFu);
  return 1;
}

/* Same affine math as CircleOS TOUCHSCR_CalculateCalibration(). */
static int cal_compute(const int raw[6]) {
  int dx = raw[0] - raw[4];
  int dy = raw[1] - raw[5];
  int ex = raw[2] - raw[4];
  int ey = raw[3] - raw[5];
  int delta = dx * ey - ex * dy;
  if (delta == 0) return 0;
  int a = (cal_ref[0][0] - cal_ref[2][0]) * ey - (cal_ref[1][0] - cal_ref[2][0]) * dy;
  int b = (cal_ref[1][0] - cal_ref[2][0]) * dx - (cal_ref[0][0] - cal_ref[2][0]) * ex;
  int c = cal_ref[2][0] * delta - a * raw[4] - b * raw[5];
  int d = (cal_ref[0][1] - cal_ref[2][1]) * ey - (cal_ref[1][1] - cal_ref[2][1]) * dy;
  int e = (cal_ref[1][1] - cal_ref[2][1]) * dx - (cal_ref[0][1] - cal_ref[2][1]) * ex;
  int f = cal_ref[2][1] * delta - d * raw[4] - e * raw[5];
  cal_a = a; cal_b = b; cal_c = c;
  cal_d = d; cal_e = e; cal_f = f;
  cal_r = delta;
  cal_valid = 1;
  return 1;
}

int board_touch_calib_set(const int raw[6]) {
  if (!cal_compute(raw)) return 0;
  cal_save_raw(raw);
  return 1;
}
int board_touch_calib_points(int raw[6]) { return cal_load_raw(raw); }
int board_touch_calib_valid(void) { return cal_valid; }

int board_touch_raw(int *x, int *y) {
  ADC3_CR2 |= 1u << 21;                    /* JSWSTART */
  int t = 0;
  while (!(ADC3_SR & 4u) && ++t < 20000) { }
  if (!(ADC3_SR & 4u)) return 0;
  ADC3_SR &= ~4u;
  unsigned l = ADC3_JDR1 & 0xfff;
  unsigned u = ADC3_JDR3 & 0xfff;
  unsigned d = ADC3_JDR4 & 0xfff;
  ADC3_CR2 &= ~(1u << 21);
  if (u >= 0xc00u) return 0;               /* not pressed */
  int ir1 = 4096 - (int)u;
  if (ir1 < 200) return 0;
  int rx = (4000 * (int)l) / ir1;          /* official CircleOS 0..4000 */
  int ry = (4000 * ((int)u - (int)d)) / ir1;
  if (rx < 0) rx = 0;
  if (rx > 4095) rx = 4095;
  if (ry < 0) ry = 0;
  if (ry > 4095) ry = 4095;
  *x = rx;
  *y = ry;
  return 1;
}

int board_touch_read(int *x, int *y) {
  int rx, ry;
  if (!board_touch_raw(&rx, &ry)) return 0;
  int px, py;
  if (cal_valid && cal_r) {
    px = (cal_a * rx + cal_b * ry + cal_c) / cal_r;
    py = (cal_d * rx + cal_e * ry + cal_f) / cal_r;
  } else {
    /* Uncalibrated fallback: Chopstx map from the 0..1000 scale. */
    int x1000 = rx / 4, y1000 = ry / 4;
    px = (128 * (x1000 - 0x20)) / 0x100;
    py = (160 * (0x1e0 - y1000)) / 0x1c0;
  }
  if (px < 0) px = 0;
  if (px >= 128) px = 127;
  if (py < 0) py = 0;
  if (py >= 160) py = 159;
  *x = px;
  *y = py;
  return 1;
}

void board_init(void) {
  clock_init();
  /* enable DWT cycle counter (perf meter) */
  DEMCR |= (1u << 24);      /* TRCENA */
  DWT_CYCCNT = 0;
  DWT_CTRL |= 1u;           /* CYCCNTENA */
  pfm_prof_clock = board_cycles; /* enable per-task CPU profiling */
  /* GPIOA,B,D,E + AFIO clocks */
  RCC_APB2ENR |= (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6);

  /* LEDs */
  pin_cfg(GPIOE_BASE, PRIMER2_LED0_PIN, CNF_OUT_PP_50);
  pin_cfg(GPIOE_BASE, PRIMER2_LED1_PIN, CNF_OUT_PP_50);
  pin_set(GPIOE_BASE, PRIMER2_LED1_PIN, 1); /* alive indicator */

  /* joystick inputs, pull-down */
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_LEFT_PIN, CNF_IN_PULL);
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_UP_PIN, CNF_IN_PULL);
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_RIGHT_PIN, CNF_IN_PULL);
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_DOWN_PIN, CNF_IN_PULL);
  pin_cfg(GPIOA_BASE, PRIMER2_JOY_CENTER_PIN, CNF_IN_PULL);
  GPIO_ODR(GPIOE_BASE) &= ~((1u << PRIMER2_JOY_LEFT_PIN) | (1u << PRIMER2_JOY_UP_PIN) |
                            (1u << PRIMER2_JOY_RIGHT_PIN) | (1u << PRIMER2_JOY_DOWN_PIN));
  GPIO_ODR(GPIOA_BASE) &= ~(1u << PRIMER2_JOY_CENTER_PIN);

  mems_init();      /* LIS3LV02 on SPI1 (optional) */
  board_touch_init(); /* ADC3 4-wire touch (PC0..PC3) */

  backlight_init(); /* TIM4 PWM on PB8 */

  fsmc_init();
  lcd_controller_init();

  pwr_init();
}
