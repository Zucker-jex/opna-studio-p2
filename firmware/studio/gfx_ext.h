#ifndef GFX_EXT_H
#define GFX_EXT_H

#include <stdint.h>

/* Small drawing helpers on top of the board LCD HAL + gfx text renderer. */
#define RGB(r, g, b) \
  ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

void gx_fill(int x, int y, int w, int h, uint16_t c);
uint16_t gx_mix(uint16_t a, uint16_t b, int t);
void gx_hline(int x, int y, int w, uint16_t c);
void gx_vline(int x, int y, int h, uint16_t c);
void gx_rect(int x, int y, int w, int h, uint16_t c);
void gx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gx_grad_v(int x, int y, int w, int h, uint16_t top, uint16_t bot);
void gx_bar(int x, int y, int w, int h, int val, int max, uint16_t fg, uint16_t bg);
void gx_text_c(int cx, int y, const char *s, uint16_t fg, uint16_t bg);
void gx_text_r(int xr, int y, const char *s, uint16_t fg, uint16_t bg);
void gx_text_sh(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint16_t shadow);
void gx_blit(int x, int y, int w, int h, const uint8_t *rows, uint16_t fg, uint16_t bg);
void gx_seg_bar(int x, int y, int w, int h, int val, int max, uint16_t fg, uint16_t bg);
void gx_vu(int x, int y, int w, int h, int lvl, uint16_t c);

#endif
