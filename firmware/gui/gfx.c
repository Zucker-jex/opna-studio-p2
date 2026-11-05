#include "gfx.h"
#include "board.h"
#include "font_sjis.h"

/* One unscaled glyph is at most 8x8; scale is 1, 2 or 4.  Building the whole
   scaled glyph and handing it to board_lcd_blit() turns 32 (or 128) separate
   LCD window writes per character into one, which matters at 20 fps. */
static uint16_t g_glyph[32 * 32];

static void draw_glyph(int x, int y, const uint8_t rows[8], int w,
                       uint16_t fg, uint16_t bg, int scale) {
  if (scale < 1) scale = 1;
  if (scale > 4) scale = 4;
  int gw = w * scale, gh = SJIS_CELL_H * scale;
  for (int ry = 0; ry < SJIS_CELL_H; ry++) {
    uint8_t bits = rows[ry];
    for (int rx = 0; rx < w; rx++) {
      uint16_t c = (bits & (0x80 >> rx)) ? fg : bg;
      for (int dy = 0; dy < scale; dy++) {
        uint16_t *d = &g_glyph[(ry * scale + dy) * gw + rx * scale];
        for (int dx = 0; dx < scale; dx++) d[dx] = c;
      }
    }
  }
  board_lcd_blit(x, y, gw, gh, g_glyph);
}

static int is_lead(unsigned b) {
  return (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xef);
}
static int is_trail(unsigned b) {
  return (b >= 0x40 && b <= 0x7e) || (b >= 0x80 && b <= 0xfc);
}

int gfx_text_s(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned b = *p;
    if (is_lead(b) && is_trail(p[1])) {
      unsigned b2 = p[1];
      unsigned lead = (b <= 0x9f) ? b - 0x81 : b - 0xc1;
      unsigned trail = (b2 <= 0x7e) ? b2 - 0x40 : b2 - 0x41;
      unsigned idx = lead * SJIS_ZEN_NTRAIL + trail;
      if (idx < (unsigned)(SJIS_ZEN_NLEAD * SJIS_ZEN_NTRAIL))
        draw_glyph(x, y, sjis_zenkaku[idx], 8, fg, bg, scale);
      x += 8 * scale;
      p += 2;
    } else {
      const uint8_t *g = (b >= SJIS_HANK_FIRST && b <= SJIS_HANK_LAST)
                             ? sjis_hankaku[b - SJIS_HANK_FIRST]
                             : sjis_hankaku[0];
      draw_glyph(x, y, g, 4, fg, bg, scale);
      x += 4 * scale;
      p += 1;
    }
  }
  return x;
}

int gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  return gfx_text_s(x, y, s, fg, bg, 1);
}

int gfx_width(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  int w = 0;
  while (*p) {
    if (is_lead(*p) && is_trail(p[1])) { w += 8; p += 2; }
    else { w += 4; p += 1; }
  }
  return w;
}
