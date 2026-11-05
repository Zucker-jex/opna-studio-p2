#include "gfx_ext.h"
#include "gfx.h"
#include "board.h"

void gx_fill(int x, int y, int w, int h, uint16_t c) {
  board_lcd_fill_rect(x, y, w, h, c);
}

void gx_hline(int x, int y, int w, uint16_t c) { board_lcd_fill_rect(x, y, w, 1, c); }
void gx_vline(int x, int y, int h, uint16_t c) { board_lcd_fill_rect(x, y, 1, h, c); }

void gx_rect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  gx_hline(x, y, w, c);
  gx_hline(x, y + h - 1, w, c);
  gx_vline(x, y, h, c);
  gx_vline(x + w - 1, y, h, c);
}

void gx_line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = x1 - x0, dy = y1 - y0;
  int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
  dx = dx < 0 ? -dx : dx;
  dy = dy < 0 ? -dy : dy;
  int err = dx - dy;
  for (;;) {
    board_lcd_pixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err * 2;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx) { err += dx; y0 += sy; }
  }
}

uint16_t gx_mix(uint16_t a, uint16_t b, int t) {
  int ar = (a >> 11) & 0x1f, ag = (a >> 5) & 0x3f, ab = a & 0x1f;
  int br = (b >> 11) & 0x1f, bg = (b >> 5) & 0x3f, bb = b & 0x1f;
  int r = ar + (br - ar) * t / 255;
  int g = ag + (bg - ag) * t / 255;
  int bl = ab + (bb - ab) * t / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

void gx_grad_v(int x, int y, int w, int h, uint16_t top, uint16_t bot) {
  if (h <= 1) { gx_fill(x, y, w, h, top); return; }
  for (int i = 0; i < h; i++)
    board_lcd_fill_rect(x, y + i, w, 1, gx_mix(top, bot, i * 255 / (h - 1)));
}

void gx_bar(int x, int y, int w, int h, int val, int max, uint16_t fg, uint16_t bg) {
  if (w <= 0 || h <= 0) return;
  gx_fill(x, y, w, h, bg);
  if (max <= 0) return;
  if (val < 0) val = 0;
  if (val > max) val = max;
  int fw = w * val / max;
  if (fw > 0) gx_fill(x, y, fw, h, fg);
}

void gx_seg_bar(int x, int y, int w, int h, int val, int max, uint16_t fg, uint16_t bg) {
  /* segmented VU-style bar: one 2px block per unit with a 1px gap */
  int n = max > 0 ? max : 1;
  int bw = w / n - 1;
  if (bw < 1) bw = 1;
  for (int i = 0; i < n; i++) {
    int bx = x + i * (bw + 1);
    if (bx + bw > x + w) break;
    gx_fill(bx, y, bw, h, i < val ? fg : bg);
  }
}

void gx_text_c(int cx, int y, const char *s, uint16_t fg, uint16_t bg) {
  int w = gfx_width(s);
  gfx_text(cx - w / 2, y, s, fg, bg);
}

void gx_text_r(int xr, int y, const char *s, uint16_t fg, uint16_t bg) {
  gfx_text(xr - gfx_width(s), y, s, fg, bg);
}

void gx_text_sh(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint16_t sh) {
  gfx_text(x + 1, y + 1, s, sh, bg);
  gfx_text(x, y, s, fg, bg);
}

void gx_blit(int x, int y, int w, int h, const uint8_t *rows, uint16_t fg, uint16_t bg) {
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++)
      board_lcd_pixel(x + c, y + r, (rows[r] & (0x80 >> c)) ? fg : bg);
}

void gx_vu(int x, int y, int w, int h, int lvl, uint16_t c) {
  /* vertical bar growing from the bottom; lvl 0..100 */
  if (lvl < 0) lvl = 0;
  if (lvl > 100) lvl = 100;
  int fh = h * lvl / 100;
  gx_fill(x, y, w, h, RGB(16, 20, 34));
  for (int i = 0; i < fh; i += 2) gx_hline(x, y + h - 1 - i, w, c);
}
