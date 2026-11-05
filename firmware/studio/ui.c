/*
 * OPNA Studio P2 — pixel-art UI.
 *
 * 128x160, five pages (SEQ / SYN / MIX / VIS / SYS), 4x8/8x8 Misaki font.
 * The layout keeps the pocket-fmdsp visual language: dark navy, a gold title
 * rail, cyan values, a red selection, and a bottom tab bar.
 */
#include "ui.h"
#include "studio.h"
#include "app.h"
#include "patches.h"
#include "notes.h"
#include "gfx.h"
#include "gfx_ext.h"
#include "board.h"

#include <string.h>

#ifdef PFM_RTOS
#include "pwr_support.h"
#include "FreeRTOS.h"
#include "task.h"
#endif

#define W 128
#define H 160

#ifdef PFM_RTOS
/* VIS spectrum/scope is rendered off-screen and flushed in one window; the
   panel never sees the blank intermediate frame (the flicker). */
static uint16_t vis_cv[W * 73];
#endif

/* ------------------------------------------------------------------------- */
/* palette                                                                   */
/* ------------------------------------------------------------------------- */
#define C_BG      RGB(8, 10, 18)
#define C_BG2     RGB(12, 15, 27)
#define C_PANEL   RGB(20, 26, 46)
#define C_PANEL2  RGB(30, 38, 66)
#define C_LINE    RGB(42, 52, 86)
#define C_TEXT    RGB(214, 222, 243)
#define C_DIM     RGB(126, 138, 170)
#define C_DIMMER  RGB(68, 78, 106)
#define C_GOLD    RGB(255, 212, 94)
#define C_CYAN    RGB(99, 217, 255)
#define C_GREEN   RGB(99, 242, 166)
#define C_RED     RGB(255, 92, 110)
#define C_ORANGE  RGB(255, 155, 69)
#define C_YELLOW  RGB(255, 232, 94)
#define C_PURPLE  RGB(184, 140, 255)
#define C_WHITE   RGB(255, 255, 255)
#define C_BLACK   RGB(0, 0, 0)

static const uint16_t trk_col[ST_TRACKS] = {
  RGB(255,212,94), RGB(255,179,92), RGB(255,126,138), RGB(192,140,255),
  RGB(127,212,255), RGB(107,255,176), RGB(102,224,255), RGB(79,214,200),
  RGB(182,255,107), RGB(255,107,74),
};
static const uint16_t page_accent[PG_N] = { C_GOLD, C_GREEN, C_ORANGE, C_CYAN, C_PURPLE };
static const char *const page_name[PG_N] = { "SEQ", "SYN", "MIX", "VIS", "SYS" };

/* ------------------------------------------------------------------------- */
/* UI state                                                                  */
/* ------------------------------------------------------------------------- */
struct ui_state {
  int page;
  int seq_step, seq_track;
  int synth_key, synth_oct;
  int mix_sel;
  int vis_mode;
  int sys_sel;
  unsigned flash;          /* 0 none, 1 pat, 2 bpm, 3 save ok, 4 save err */
  int flash_val;
  uint32_t flash_until;
  uint32_t gen;
  int mod;
  uint32_t mod_since;
  int cal, cal_step, cal_msg;
  uint32_t cal_since;
  int cal_raw[6];
};

struct ui_view {
  int page, seq_step, seq_track, synth_key, synth_oct, mix_sel, vis_mode, sys_sel;
  unsigned flash;
  int flash_val;
  uint32_t flash_until;
  uint32_t gen;
  int mod;
  int cal, cal_step, cal_msg;
};

static struct ui_state U;
static struct ui_view V;        /* draw snapshot */
static struct ui_view V_live;   /* latest published */

/* Dirty rectangles for the key-interaction repaint.  With a clip list the
   full page draw runs but only these rectangles reach GRAM, so a cursor move
   does not rewrite the whole screen. */
#define UI_DIRTY_MAX 8
static int16_t dirty_r[UI_DIRTY_MAX][4];
static int dirty_n;

static void syn_key_geom(int key, int *x, int *w);

static void ui_invalidate(int x, int y, int w, int h) {
  if (dirty_n >= UI_DIRTY_MAX || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  dirty_r[dirty_n][0] = (int16_t)x; dirty_r[dirty_n][1] = (int16_t)y;
  dirty_r[dirty_n][2] = (int16_t)w; dirty_r[dirty_n][3] = (int16_t)h;
  dirty_n++;
}
int ui_dirty_pending(void) { return dirty_n > 0; }

static const uint8_t def_pencil[ST_TRACKS] = { 45, 69, 57, 60, 64, 76, 81, 57, 0, 0 };
static const uint8_t def_vel[ST_TRACKS]    = { 14, 14, 12, 12, 12, 12, 12, 11, 10, 12 };
static const uint8_t def_gate[ST_TRACKS]   = { 7, 9, 15, 4, 3, 5, 2, 11, 2, 2 };
static uint8_t pencil[ST_TRACKS];

static const uint16_t drum_col[6] = { C_RED, C_ORANGE, C_GREEN, C_YELLOW, C_PURPLE, C_CYAN };

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */
static void u2s(char *d, int v, int width) {
  char t[8];
  int n = 0;
  unsigned uv = v < 0 ? (unsigned)(-v) : (unsigned)v;
  do { t[n++] = (char)('0' + uv % 10); uv /= 10; } while (uv);
  int i = 0;
  if (v < 0) d[i++] = '-';
  while (i < width - n) d[i++] = ' ';
  while (n) d[i++] = t[--n];
  d[i] = 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Single cross-task publication point.  Every field the LCD reads from V
   MUST be copied here; a missing field silently keeps the previous frame. */
static void publish(void) {
  V_live.page = U.page;
  V_live.seq_step = U.seq_step;
  V_live.seq_track = U.seq_track;
  V_live.synth_key = U.synth_key;
  V_live.synth_oct = U.synth_oct;
  V_live.mix_sel = U.mix_sel;
  V_live.vis_mode = U.vis_mode;
  V_live.sys_sel = U.sys_sel;
  V_live.flash = U.flash;
  V_live.flash_val = U.flash_val;
  V_live.flash_until = U.flash_until;
  V_live.mod = U.mod;
  V_live.cal = U.cal;
  V_live.cal_step = U.cal_step;
  V_live.cal_msg = U.cal_msg;
  V_live.gen++;
}

static void ui_snapshot(void) {
#ifdef PFM_RTOS
  taskENTER_CRITICAL();
#endif
  V = V_live;
#ifdef PFM_RTOS
  taskEXIT_CRITICAL();
#endif
}

static void flash_set(unsigned kind, int val, uint32_t now) {
  U.flash = kind;
  U.flash_val = val;
  U.flash_until = now + 1200;
  ui_invalidate(0, 137, W, 11);
}

static int synth_note(void) {
  int n = (U.synth_oct + 1) * 12 + U.synth_key;
  return clampi(n, 0, 127);
}

/* ------------------------------------------------------------------------- */
/* chrome                                                                    */
/* ------------------------------------------------------------------------- */
static void draw_status_cpu(void) {
  char b[12];
  uint32_t cpu = g_app.cpu_pct;
  gx_fill(80, 0, 48, 8, C_BG);
  strcpy(b, "CPU ");
  u2s(b + 4, (int)cpu, 3);
  int l = (int)strlen(b);
  b[l] = '%'; b[l + 1] = 0;
  gx_text_r(126, 0, b, cpu > 90 ? C_RED : (cpu > 70 ? C_YELLOW : C_GREEN), C_BG);
}

static void draw_status(void) {
  gx_fill(0, 0, W, 8, C_BG);
  gfx_text(2, 0, "OPNA.P2", C_GOLD, C_BG);
  if (studio_playing()) {
    static const uint8_t tri[5] = { 0x10, 0x18, 0x1C, 0x18, 0x10 };
    gx_blit(38, 1, 5, 5, tri, C_GREEN, C_BG);
    gfx_text(45, 0, "PLAY", C_GREEN, C_BG);
  } else {
    gfx_text(40, 0, "STOP", C_DIMMER, C_BG);
  }
  draw_status_cpu();
  gx_hline(0, 8, W, page_accent[V.page]);
}

static void draw_header(const char *title, uint16_t accent,
                        const char *l1, const char *l2) {
  gx_fill(0, 9, W, 17, C_PANEL);
  gx_hline(0, 9, W, C_PANEL2);
  gfx_text_s(5, 11, title, C_BLACK, C_PANEL, 2);
  gfx_text_s(4, 10, title, accent, C_PANEL, 2);
  if (l1) gx_text_r(124, 10, l1, C_TEXT, C_PANEL);
  if (l2) gx_text_r(124, 19, l2, C_DIM, C_PANEL);
  gx_hline(0, 26, W, accent);
}

static void draw_tabs(void) {
  static const int tx[6] = { 0, 25, 50, 75, 100, 128 };
  gx_fill(0, 148, W, 12, C_BG2);
  gx_hline(0, 147, W, C_LINE);
  for (int p = 0; p < PG_N; p++) {
    int x = tx[p], w = tx[p + 1] - tx[p];
    int on = (p == V.page);
    if (on) {
      gx_fill(x, 148, w, 12, gx_mix(page_accent[p], C_PANEL, 55));
      gx_hline(x, 148, w, page_accent[p]);
      gx_hline(x, 149, w, page_accent[p]);
    }
    gx_text_c(x + w / 2, 151, page_name[p],
              on ? page_accent[p] : C_DIMMER, on ? C_PANEL : C_BG2);
  }
}

static const char *const hints[PG_N][4] = {
  { "UD track  LR step", "C dot  2xC play", "3xC MOD  UD semi LR oct", "holdC next page" },
  { "LR key  UD octave", "C play  2xC play", "3xC MOD  UD patch", "holdC next page" },
  { "UD track  LR level", "C mute  2xC play", "master row = volume", "holdC next page" },
  { "C mode  2xC play", "FFT / scope", "holdC next page", "CPU headroom" },
  { "UD row   LR value", "C act  2xC play", "Touch Cal row", "holdC next page" },
};

static void draw_hint(uint32_t now) {
  gx_fill(0, 137, W, 11, C_BG);
  gx_hline(0, 137, W, C_LINE);
  const char *s;
  static char tmp[26];
  if (V.mod) {
    if (V.page == PG_SEQ) s = "MOD  UD semi  LR octave";
    else if (V.page == PG_SYN) s = "MOD  UD patch  LR arp/lat";
    else s = "MOD";
  } else if (V.flash_until > now) {
    static const char *const fl[5] = { "", "PAT ", "BPM ", "SAVE ", "SAVE " };
    tmp[0] = 0;
    if (V.flash == 3) { strcpy(tmp, "SAVED TO SD"); }
    else if (V.flash == 4) { strcpy(tmp, "SAVE ERROR"); }
    else {
      strcpy(tmp, fl[V.flash]);
      char n[8];
      if (V.flash == 1) { n[0] = (char)('A' + V.flash_val); n[1] = 0; }
      else u2s(n, V.flash_val, 1);
      strcat(tmp, n);
    }
    s = tmp;
  } else {
    s = hints[V.page][(now / 3500) & 3];
  }
  gx_fill(3, 141, 2, 2, page_accent[V.page]);
  gfx_text(8, 139, s, C_DIM, C_BG);
}

/* ------------------------------------------------------------------------- */
/* SEQ page                                                                  */
/* ------------------------------------------------------------------------- */
static void seq_toggle(void) {
  struct st_song *s = studio_song();
  int p = studio_pattern(), t = U.seq_track, u = U.seq_step;
  struct st_step *st = &s->step[p][t][u];
  if (t == ST_TRK_DRUM) {
    if (st->note == 0xff) { st->note = 0; st->vel = 12; st->gate = 2; }
    else if (st->note >= 5) st->note = 0xff;
    else st->note++;
    if (st->note != 0xff) studio_preview(t, st->note);
  } else if (st->note == 0xff) {
    st->note = pencil[t];
    st->vel = def_vel[t];
    st->gate = def_gate[t];
    studio_preview(t, st->note);
  } else {
    st->note = 0xff;
  }
  publish();
}

static void seq_transpose(int dir) {
  int t = U.seq_track;
  if (t == ST_TRK_DRUM) return;
  int n = clampi((int)pencil[t] + dir, 0, 127);
  pencil[t] = (uint8_t)n;
  struct st_step *st = &studio_song()->step[studio_pattern()][t][U.seq_step];
  if (st->note != 0xff) {
    st->note = (uint8_t)clampi((int)st->note + dir, 0, 127);
    studio_preview(t, st->note);
  } else {
    studio_preview(t, n);
  }
  publish();
}

static void seq_octave(int dir) {
  int t = U.seq_track;
  if (t == ST_TRK_DRUM) return;
  int n = clampi((int)pencil[t] + 12 * dir, 0, 127);
  pencil[t] = (uint8_t)n;
  struct st_step *st = &studio_song()->step[studio_pattern()][t][U.seq_step];
  if (st->note != 0xff) {
    st->note = (uint8_t)clampi((int)st->note + 12 * dir, 0, 127);
    studio_preview(t, st->note);
  } else {
    studio_preview(t, n);
  }
  publish();
}

static void draw_seq_ruler(int step, int playing) {
  for (int s = 0; s < ST_STEPS; s++) {
    int x = 26 + s * 6;
    uint16_t c = C_PANEL;
    if ((s & 3) == 0) c = C_LINE;
    if (playing && s == step) c = C_GOLD;
    gx_fill(x, 38, 5, 3, c);
  }
  gx_hline(26, 41, 96, C_LINE);
  if (playing) {
    int x = 26 + step * 6 + 2;
    gx_fill(x, 38, 2, 3, C_WHITE);
  }
}

static void draw_seq_header_right(int p) {
  char b[8], bpm[16];
  strcpy(b, "BAR ");
  { char n[4]; u2s(n, p + 1, 1); strcat(b, n); }
  strcpy(bpm, "BPM ");
  { char n[4]; u2s(n, studio_bpm(), 1); strcat(bpm, n); }
  gx_fill(88, 9, 40, 17, C_PANEL);
  gx_hline(88, 9, 40, C_PANEL2);
  gx_text_r(124, 10, b, C_TEXT, C_PANEL);
  gx_text_r(124, 19, bpm, C_DIM, C_PANEL);
}

static void draw_seq_pattern_bar(int p) {
  char b[24];
  gx_fill(0, 28, W, 9, C_BG);
  gfx_text(2, 29, "PAT", C_DIM, C_BG);
  for (int i = 0; i < 4; i++) {
    int x = 18 + i * 11, on = (i == p);
    gx_fill(x, 28, 10, 8, on ? C_GOLD : C_PANEL);
    gfx_text(x + 3, 29, (const char[]){ (char)('A' + i), 0 }, on ? C_BG : C_DIM,
             on ? C_GOLD : C_PANEL);
  }
  gfx_text(67, 29, "SW", C_DIM, C_BG);
  u2s(b, studio_swing(), 2);
  gfx_text(79, 29, b, C_CYAN, C_BG);
  gfx_text(94, 29, studio_chain() ? "CHN" : "---", studio_chain() ? C_GREEN : C_DIMMER, C_BG);
}

static void draw_seq_grid(int p) {
  struct st_song *song = studio_song();
  for (int t = 0; t < ST_TRACKS; t++) {
    int y = 42 + t * 8;
    int sel = (t == V.seq_track);
    gx_fill(0, y, 26, 8, sel ? C_PANEL : C_BG);
    gx_fill(0, y + 1, 2, 6, song->mute[t] ? C_DIMMER : trk_col[t]);
    gfx_text(4, y + 1, st_track_name[t],
             song->mute[t] ? C_DIMMER : (sel ? C_WHITE : C_DIM), sel ? C_PANEL : C_BG);
    if (song->mute[t]) gfx_text(20, y + 1, "M", C_RED, sel ? C_PANEL : C_BG);
    for (int s = 0; s < ST_STEPS; s++) {
      int x = 26 + s * 6;
      struct st_step *st = &song->step[p][t][s];
      gx_fill(x, y, 6, 8, C_BG);
      if (st->note == 0xff) {
        gx_fill(x + 3, y + 4, 1, 1, C_LINE);
      } else {
        uint16_t c = (t == ST_TRK_DRUM) ? drum_col[st->note % 6] : trk_col[t];
        if (!song->mute[t]) {
          if (st->vel < 8) c = gx_mix(c, C_BG, 130);
          else if (st->vel < 11) c = gx_mix(c, C_BG, 60);
        } else c = C_DIMMER;
        gx_fill(x + 1, y + 1, 4, 6, c);
      }
    }
  }
  int x = 26 + V.seq_step * 6, y = 42 + V.seq_track * 8;
  gx_rect(x - 1, y - 1, 8, 10, C_WHITE);
}

static void draw_seq_footer(int p) {
  char b[24];
  struct st_song *song = studio_song();
  int t = V.seq_track, s = V.seq_step;
  struct st_step *st = &song->step[p][t][s];
  gx_fill(0, 124, W, 13, C_BG);
  b[0] = 0;
  if (t == ST_TRK_DRUM) {
    strcpy(b, "DRM ");
    if (st->note == 0xff) strcat(b, "--");
    else strcat(b, st_drum_name[st->note % 6]);
  } else {
    const char *nm = studio_note_name(st->note == 0xff ? pencil[t] : st->note);
    strcat(b, nm);
    if (st->note == 0xff) strcat(b, " .");
    else {
      strcat(b, " v");
      char n[3]; u2s(n, st->vel, 1); strcat(b, n);
    }
  }
  gfx_text(2, 124, b, C_TEXT, C_BG);
  gfx_text(2, 133, "NOTE", C_DIMMER, C_BG);
  if (t == ST_TRK_DRUM) {
    char n[3]; u2s(n, V.seq_step + 1, 2);
    gfx_text(30, 133, "STEP", C_DIM, C_BG);
    gfx_text(52, 133, n, C_CYAN, C_BG);
  } else {
    const char *nm = studio_note_name(pencil[t]);
    gfx_text(22, 133, nm, C_CYAN, C_BG);
    gfx_text(40, 133, st_track_name[t], C_DIM, C_BG);
  }
  gx_text_r(126, 124, "STEP", C_DIMMER, C_BG);
  {
    char n[3]; u2s(n, s + 1, 2);
    gx_text_r(126, 133, n, C_CYAN, C_BG);
  }
}

static void draw_seq(void) {
  int p = studio_pattern();
  int step = studio_step();
  int playing = studio_playing();
  char b[8], bpm[16];
  strcpy(b, "BAR ");
  { char n[4]; u2s(n, p + 1, 1); strcat(b, n); }
  strcpy(bpm, "BPM ");
  { char n[4]; u2s(n, studio_bpm(), 1); strcat(bpm, n); }
  draw_header("SEQ", C_GOLD, b, bpm);
  draw_seq_pattern_bar(p);
  draw_seq_ruler(step, playing);
  draw_seq_grid(p);
  draw_seq_footer(p);
}

/* Pattern-chain change: repaint only the SEQ regions that changed, not the
   status/tabs/hint or the whole screen. */
static void pattern_body(void) {
  int p = studio_pattern();
  draw_seq_header_right(p);
  draw_seq_pattern_bar(p);
  draw_seq_ruler(studio_step(), studio_playing());
  draw_seq_grid(p);
  draw_seq_footer(p);
}

void ui_redraw_pattern(void) { ui_snapshot(); pattern_body(); }

/* ------------------------------------------------------------------------- */
/* SYN page                                                                  */
/* ------------------------------------------------------------------------- */
static void synth_patch_shift(int dir) {
  int p = (studio_live_patch_get() + dir + ST_PATCHES) % ST_PATCHES;
  studio_live_patch(p);
  publish();
}

static void draw_badge(int x, int y, int w, int h, const char *s, int on, uint16_t col) {
  gx_fill(x, y, w, h, on ? col : C_PANEL);
  gx_text_c(x + w / 2, y + (h - 8) / 2, s, on ? C_BG : C_DIMMER, on ? col : C_PANEL);
}

static void draw_syn_tilt(void) {
  gx_fill(100, 28, 27, 27, C_BG2);
  gx_rect(100, 28, 27, 27, C_LINE);
  gx_vline(113, 29, 25, C_LINE);
  gx_hline(101, 41, 25, C_LINE);
  int tx = clampi(studio_live_tilt_x(), -100, 100);
  int ty = clampi(studio_live_tilt_y(), -100, 100);
  int dx = 113 + tx * 11 / 100;
  int dy = 41 + ty * 11 / 100;
  gx_fill(dx - 1, dy - 1, 3, 3, C_CYAN);
  gfx_text(101, 57, "TILT", C_DIM, C_BG);
}

static void draw_syn_tilt_anim(void) {
  static int ox = -1, oy = -1;
  int tx = clampi(studio_live_tilt_x(), -100, 100);
  int ty = clampi(studio_live_tilt_y(), -100, 100);
  int dx = 113 + tx * 11 / 100;
  int dy = 41 + ty * 11 / 100;
  if (dx == ox && dy == oy) return;
  if (ox >= 0) {
    gx_fill(ox - 1, oy - 1, 3, 3, C_BG2);
    gx_vline(113, oy - 1, 3, C_LINE);
    gx_hline(ox - 1, 41, 3, C_LINE);
  }
  gx_fill(dx - 1, dy - 1, 3, 3, C_CYAN);
  ox = dx; oy = dy;
}

static void draw_syn_keys_body(void) {
  static const uint8_t wb[12] = { 0, 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6 };
  static const uint8_t whites[7] = { 0, 2, 4, 5, 7, 9, 11 };
  static const uint8_t blacks[5] = { 1, 3, 6, 8, 10 };
  int base = (V.synth_oct + 1) * 12;
  int live = studio_live_note();
  int pi = studio_live_patch_get();
  gx_hline(0, 99, 128, C_LINE);
  /* naturals */
  for (int oct = 0; oct < 2; oct++) {
    for (int w = 0; w < 7; w++) {
      int x = 1 + (oct * 7 + w) * 9;
      int note = base + oct * 12 + whites[w];
      uint16_t c = RGB(198, 208, 230);
      if (note == live) c = C_GREEN;
      else if (note == base + V.synth_key) c = C_GOLD;
      gx_fill(x, 100, 8, 27, c);
      gx_hline(x, 100, 8, C_LINE);
      gx_vline(x + 8, 100, 27, C_BG);
      if (whites[w] == 0) {
        char nb[3];
        nb[0] = 'C';
        nb[1] = (char)('0' + V.synth_oct + oct);
        nb[2] = 0;
        gfx_text(x + 1, 118, nb, c == RGB(198, 208, 230) ? C_BG : C_BLACK, c);
      }
    }
  }
  /* accidentals */
  for (int oct = 0; oct < 2; oct++) {
    for (int b = 0; b < 5; b++) {
      int semi = blacks[b];
      int boundary = 1 + (oct * 7 + wb[semi] + 1) * 9;
      int x = boundary - 3;
      int note = base + oct * 12 + semi;
      uint16_t c = RGB(28, 34, 56);
      if (note == live) c = C_GREEN;
      else if (note == base + V.synth_key) c = C_GOLD;
      gx_fill(x, 100, 5, 17, c);
      gx_hline(x, 100, 5, c == RGB(28, 34, 56) ? C_LINE : C_BG);
    }
  }
  /* cursor */
  {
    int semi = V.synth_key % 12;
    int oct = V.synth_key / 12;
    int x, w, h;
    if (semi == 1 || semi == 3 || semi == 6 || semi == 8 || semi == 10) {
      x = 1 + (oct * 7 + wb[semi] + 1) * 9 - 3;
      w = 5; h = 17;
    } else {
      int ww = 0;
      while (ww < 7 && whites[ww] != semi) ww++;
      x = 1 + (oct * 7 + ww) * 9;
      w = 8; h = 27;
    }
    gx_fill(x, 100, w, 2, C_WHITE);
    gx_rect(x - 1, 99, w + 2, h + 1, C_WHITE);
  }
  /* info line */
  const char *nm = studio_note_name(base + V.synth_key);
  gfx_text(2, 129, nm, C_TEXT, C_BG);
  gfx_text(20, 129, "KEY", C_DIMMER, C_BG);
  if (studio_live_active()) gfx_text(38, 129, "LIVE", C_GREEN, C_BG);
  else if (studio_live_arp_get()) gfx_text(38, 129, "ARP", C_CYAN, C_BG);
  char n[8];
  strcpy(n, "PATCH ");
  u2s(n + 6, pi + 1, 1);
  strcat(n, "/8");
  gx_text_r(126, 129, n, C_DIM, C_BG);
}

static void draw_syn_keys(void) {
  gx_fill(0, 99, 128, 38, C_BG);
  draw_syn_keys_body();
}

/* Live-note change: repaint only the old/new key and the info line. */
static void draw_syn_keys_anim(void) {
  static int last = -1;
  int now = studio_live_note();
  if (now == last) return;
  int base = (V.synth_oct + 1) * 12;
  board_lcd_clip_reset();
  if (last >= base && last < base + 24) {
    int x, w; syn_key_geom(last - base, &x, &w);
    board_lcd_clip_add(x - 2, 99, w + 4, 38);
  }
  if (now >= base && now < base + 24) {
    int x, w; syn_key_geom(now - base, &x, &w);
    board_lcd_clip_add(x - 2, 99, w + 4, 38);
  }
  board_lcd_clip_add(0, 129, W, 9);
  gx_fill(0, 129, 128, 9, C_BG);
  draw_syn_keys_body();
  board_lcd_clip_reset();
  last = now;
}

static void draw_syn(void) {
  int pi = studio_live_patch_get();
  char b[24];
  strcpy(b, "OCT ");
  { char n[3]; u2s(n, V.synth_oct, 1); strcat(b, n); }
  draw_header("SYN", C_GREEN, b, st_patch_name[pi]);

  /* 8-patch grid */
  for (int i = 0; i < ST_PATCHES; i++) {
    int col = i & 1, row = i >> 1;
    int x = 2 + col * 48, y = 28 + row * 17;
    int on = (i == pi);
    gx_fill(x, y, 46, 16, on ? C_GREEN : C_PANEL);
    if (!on) gx_rect(x, y, 46, 16, C_LINE);
    gx_text_c(x + 23, y + 4, st_patch_name[i], on ? C_BG : C_DIM, on ? C_GREEN : C_PANEL);
  }

  draw_syn_tilt();
  draw_badge(100, 66, 27, 11, "ARP", studio_live_arp_get(), C_GREEN);
  draw_badge(100, 79, 27, 11, "LAT", studio_live_latch_get(), C_GREEN);
  draw_syn_keys();
}

/* ------------------------------------------------------------------------- */
/* MIX page                                                                  */
/* ------------------------------------------------------------------------- */
static void draw_mix_vu_bars(void) {
  gx_vu(102, 32, 8, 76, studio_level(0), C_GREEN);
  gx_vu(116, 32, 8, 76, studio_level(1), C_CYAN);
}

static void draw_mix_vu(void) {
  gx_fill(98, 28, 30, 88, C_BG2);
  gx_rect(98, 28, 30, 88, C_LINE);
  draw_mix_vu_bars();
  gfx_text(101, 110, "L", C_DIM, C_BG2);
  gfx_text(115, 110, "R", C_DIM, C_BG2);
}

static void draw_mix(void) {
  char b[24];
  strcpy(b, "VOL ");
  { char n[3]; u2s(n, g_app.volume, 1); strcat(b, n); }
  draw_header("MIX", C_ORANGE, b, g_app.output ? "SPKR" : "PHONES");

  struct st_song *s = studio_song();
  for (int t = 0; t < ST_TRACKS; t++) {
    int y = 28 + t * 9;
    int sel = (V.mix_sel == t);
    gx_fill(0, y, 96, 9, sel ? C_PANEL2 : C_BG);
    if (sel) gx_fill(0, y, 2, 9, C_ORANGE);
    gfx_text(4, y + 1, st_track_name[t], s->mute[t] ? C_DIMMER : (sel ? C_WHITE : C_DIM),
             sel ? C_PANEL2 : C_BG);
    gx_fill(25, y + 2, 6, 6, s->mute[t] ? C_RED : gx_mix(trk_col[t], C_BG, 110));
    gx_seg_bar(35, y + 2, 46, 5, s->level[t], 15, trk_col[t], C_PANEL);
    char n[4];
    u2s(n, s->level[t], 1);
    gfx_text(84, y + 1, n, sel ? C_WHITE : C_DIM, sel ? C_PANEL2 : C_BG);
  }
  {
    int y = 28 + 10 * 9;
    int sel = (V.mix_sel == 10);
    gx_fill(0, y, 96, 9, sel ? C_PANEL2 : C_BG);
    if (sel) gx_fill(0, y, 2, 9, C_CYAN);
    gfx_text(4, y + 1, "MASTER", sel ? C_WHITE : C_DIM, sel ? C_PANEL2 : C_BG);
    gx_seg_bar(45, y + 2, 36, 5, g_app.volume, 10, C_CYAN, C_PANEL);
    char n[4];
    u2s(n, g_app.volume, 1);
    gfx_text(84, y + 1, n, sel ? C_WHITE : C_DIM, sel ? C_PANEL2 : C_BG);
  }

  draw_mix_vu();

  gfx_text(2, 128, "UD sel  LR level  C mute", C_DIMMER, C_BG);
  { char n[8]; strcpy(n, "SD "); strcat(n, g_app.have_sd ? "OK" : "---");
    gx_text_r(126, 128, n, g_app.have_sd ? C_GREEN : C_RED, C_BG); }
}

/* ------------------------------------------------------------------------- */
/* VIS page                                                                  */
/* ------------------------------------------------------------------------- */
static void draw_vis_content(void) {
#ifdef PFM_RTOS
  board_lcd_canvas_begin(vis_cv, 0, 28, W, 73);
#endif
  const uint8_t *spec = studio_spectrum();
  const uint8_t *peak = studio_spectrum_peak();
  gx_fill(0, 28, W, 73, C_BG);
  if (V.vis_mode == 0) {
    for (int i = 1; i < 4; i++) gx_hline(0, 28 + i * 18, W, C_LINE);
    for (int d = 0; d < 64; d++) {
      int x = d * 2;
      int h = spec[d] * 72 / 63;
      int ph = peak[d] * 72 / 63;
      int top = 100 - h;
      if (h > 0) {
        int y0 = top < 64 ? 64 : top;
        gx_fill(x, y0, 1, 100 - y0, C_GREEN);
      }
      if (top < 64) {
        int y0 = top < 46 ? 46 : top;
        gx_fill(x, y0, 1, 64 - y0, C_YELLOW);
      }
      if (top < 46) {
        int y0 = top < 28 ? 28 : top;
        gx_fill(x, y0, 1, 46 - y0, C_RED);
      }
      if (ph > 0) gx_fill(x, 100 - ph, 1, 1, C_TEXT);
    }
    gx_hline(0, 100, W, C_CYAN);
  } else {
    for (int i = 1; i < 4; i++) gx_hline(0, 28 + i * 18, W, C_LINE);
    gx_hline(0, 64, W, C_LINE);
    const int16_t *sc = studio_scope();
    int prev = 64;
    for (int x = 0; x < 128; x++) {
      int v = sc[x];
      int y = 64 - v * 30 / 12000;
      if (y < 29) y = 29;
      if (y > 99) y = 99;
      int lo = y < prev ? y : prev;
      int hi = y < prev ? prev : y;
      gx_fill(x, lo, 1, hi - lo + 1, C_CYAN);
      prev = y;
    }
    gx_hline(0, 100, W, C_CYAN);
  }
#ifdef PFM_RTOS
  board_lcd_canvas_end();
#endif

  gfx_text(2, 103, "CPU", C_DIM, C_BG);
  { char n[4]; u2s(n, (int)g_app.cpu_pct, 3); gfx_text(24, 103, n, C_TEXT, C_BG); }
  gfx_text(44, 103, "%", C_DIM, C_BG);
  gfx_text(56, 103, "UND", C_DIM, C_BG);
  { char n[4]; u2s(n, (int)g_app.underruns, 3); gfx_text(76, 103, n, C_TEXT, C_BG); }
  gfx_text(96, 103, "FPS", C_DIM, C_BG);
  { char n[4]; u2s(n, (int)g_app.fps, 2); gfx_text(110, 103, n, C_TEXT, C_BG); }

  gfx_text(2, 112, "L", C_DIM, C_BG);
  gx_bar(12, 113, 78, 4, studio_level(0), 100, C_GREEN, C_PANEL);
  { char n[4]; u2s(n, studio_level(0), 3); gfx_text(94, 112, n, C_GREEN, C_BG); }
  gfx_text(2, 121, "R", C_DIM, C_BG);
  gx_bar(12, 122, 78, 4, studio_level(1), 100, C_CYAN, C_PANEL);
  { char n[4]; u2s(n, studio_level(1), 3); gfx_text(94, 121, n, C_CYAN, C_BG); }

  gfx_text(2, 129, "FM", C_DIMMER, C_BG);
  { char n[4]; u2s(n, (int)g_app.fm_pct, 2); gfx_text(12, 129, n, C_RED, C_BG); }
  gfx_text(26, 129, "SSG", C_DIMMER, C_BG);
  { char n[4]; u2s(n, (int)g_app.ssg_pct, 2); gfx_text(42, 129, n, C_GREEN, C_BG); }
  gfx_text(56, 129, "DRM", C_DIMMER, C_BG);
  { char n[4]; u2s(n, (int)g_app.drum_pct, 2); gfx_text(72, 129, n, C_ORANGE, C_BG); }
  gfx_text(88, 129, "UND", C_DIMMER, C_BG);
  { char n[4]; u2s(n, (int)g_app.underruns, 3); gfx_text(106, 129, n, C_TEXT, C_BG); }
}

static void draw_vis(void) {
  draw_header("VIS", C_CYAN, V.vis_mode ? "SCOPE" : "SPECTRUM", NULL);
  draw_vis_content();
}

/* ------------------------------------------------------------------------- */
/* SYS page                                                                  */
/* ------------------------------------------------------------------------- */
enum {
  SYS_VOL, SYS_OUT, SYS_BL, SYS_BPM, SYS_SWING, SYS_CHAIN,
  SYS_PAT, SYS_SAVE, SYS_LOAD, SYS_DEMO, SYS_MEMS, SYS_TOUCH, SYS_SHUTDOWN, SYS_N
};

static void draw_sys(void) {
  char b[24];
  strcpy(b, "SD ");
  strcat(b, g_app.have_sd ? "OK" : "NONE");
  draw_header("SYS", C_PURPLE, b, "v1.0.0");

  for (int i = 0; i < SYS_N; i++) {
    int y = 28 + i * 8;
    int sel = (V.sys_sel == i);
    gx_fill(0, y, W, 8, sel ? C_PANEL2 : C_BG);
    if (sel) gx_fill(0, y, 2, 8, C_PURPLE);
    static const char *const lab[SYS_N] = {
      "Volume", "Output", "Backlight", "Tempo", "Swing", "Chain",
      "Pattern", "Save", "Load", "Demo", "MEMS", "Touch Cal", "Shutdown",
    };
    gfx_text(4, y, lab[i], sel ? C_WHITE : C_TEXT, sel ? C_PANEL2 : C_BG);
    char v[16];
    v[0] = 0;
    switch (i) {
    case SYS_VOL: u2s(v, g_app.volume, 2); strcat(v, "/10"); break;
    case SYS_OUT: strcpy(v, g_app.output ? "Speaker" : "Phones"); break;
    case SYS_BL: u2s(v, g_app.backlight, 2); strcat(v, "/7"); break;
    case SYS_BPM: u2s(v, studio_bpm(), 3); break;
    case SYS_SWING: u2s(v, studio_swing(), 2); strcat(v, "%"); break;
    case SYS_CHAIN: strcpy(v, studio_chain() ? "On" : "Off"); break;
    case SYS_PAT: { v[0] = (char)('A' + studio_pattern()); v[1] = 0; break; }
    case SYS_SAVE:
      if (V.flash_until > g_app.uptime_ms &&
          (V.flash == 3 || V.flash == 4))
        strcpy(v, V.flash == 3 ? "OK" : "ERR");
      else strcpy(v, "Write");
      break;
    case SYS_LOAD: strcpy(v, "Read"); break;
    case SYS_DEMO: strcpy(v, "Reset"); break;
    case SYS_MEMS:
      if (!g_app.mems_ok) strcpy(v, "N/A");
      else {
        strcpy(v, "OK ");
        char n[8];
        u2s(n, g_app.mems_x, 0); strcat(v, n); strcat(v, ",");
        u2s(n, g_app.mems_y, 0); strcat(v, n);
      }
      break;
    case SYS_TOUCH: strcpy(v, "Enter"); break;
    case SYS_SHUTDOWN: strcpy(v, "Power"); break;
    }
    uint16_t vc = sel ? C_WHITE : C_CYAN;
    if (i == SYS_SAVE && v[0] == 'O') vc = C_GREEN;
    if (i == SYS_SAVE && v[0] == 'E') vc = C_RED;
    if (i == SYS_MEMS && v[0] == 'N') vc = C_DIMMER;
    gx_text_r(124, y, v, vc, sel ? C_PANEL2 : C_BG);
  }
}

/* ------------------------------------------------------------------------- */
/* boot splash                                                               */
/* ------------------------------------------------------------------------- */
void ui_splash(void) {
  gx_fill(0, 0, W, H, C_BG);
  for (int y = 4; y < H; y += 8)
    for (int x = 4; x < W; x += 8)
      gx_fill(x, y, 1, 1, RGB(26, 32, 52));

  gx_rect(0, 0, W, H, C_LINE);
  gx_rect(2, 2, W - 4, H - 4, RGB(20, 26, 44));

  gfx_text(4, 5, "PRIMER2", C_DIM, C_BG);
  gx_text_r(124, 5, "132MHz", C_DIMMER, C_BG);

  gfx_text_s(34, 30, "OPNA", C_BLACK, C_BG, 4);
  gfx_text_s(33, 29, "OPNA", C_GOLD, C_BG, 4);
  gfx_text_s(42, 64, "STUDIO", C_CYAN, C_BG, 2);
  gx_rect(104, 28, 16, 16, C_GOLD);
  gfx_text(108, 33, "P2", C_GOLD, C_BG);

  /* static waveform */
  for (int i = 0; i < 60; i++) {
    int h = (i * 7) % 13;
    if (i % 3 == 0) h = 12 - h;
    int y = 92 - h;
    gx_vline(34 + i, y, h + 1, i & 1 ? C_CYAN : RGB(52, 120, 150));
  }
  gx_hline(32, 94, 64, C_LINE);

  gx_fill(14, 110, 100, 5, C_PANEL);
  gx_fill(15, 111, 70, 3, C_CYAN);
  gx_rect(14, 110, 100, 5, C_LINE);
  gx_text_c(64, 120, "6FM + 3SSG + 6DRUM", C_DIM, C_BG);
  gx_text_c(64, 131, "55.5kHz OPNA / 132MHz M3", C_DIMMER, C_BG);
  gx_text_c(64, 144, "OPNA STUDIO v1.0.0", C_GOLD, C_BG);
}

/* ------------------------------------------------------------------------- */
/* draw dispatch                                                             */
/* ------------------------------------------------------------------------- */
static int g_painted = -1;

static const int cal_ref[3][2] = { {115, 15}, {115, 115}, {15, 15} };

static void draw_cal(void) {
  gx_fill(0, 0, W, H, C_BG);
  gfx_text(4, 2, "TOUCH CAL", C_GOLD, C_BG);
  if (V.cal == 4) {
    gfx_text(20, 60, V.cal_msg == 1 ? "SAVE OK" : "SAVE ERR",
             V.cal_msg == 1 ? C_GREEN : C_RED, C_BG);
    gfx_text(4, 90, "TOUCH CAL DONE", C_DIM, C_BG);
    gfx_text(4, 145, "C EXIT", C_DIMMER, C_BG);
    return;
  }
  for (int i = 0; i < 3; i++) {
    int x = cal_ref[i][0], y = cal_ref[i][1];
    uint16_t c = (i == V.cal_step) ? C_WHITE : (i < V.cal_step ? C_GREEN : C_DIMMER);
    gx_hline(x - 6, y, 13, c);
    gx_vline(x, y - 6, 13, c);
    gx_fill(x - 1, y - 1, 3, 3, c);
  }
  gfx_text(4, 136, "TOUCH THE CROSS", C_TEXT, C_BG);
  gfx_text(4, 148, "C CANCEL", C_DIMMER, C_BG);
  {
    char n[4];
    u2s(n, V.cal_step + 1, 1);
    gfx_text(107, 2, n, C_CYAN, C_BG);
    gfx_text(115, 2, "/3", C_DIM, C_BG);
  }
}

static void draw_page(void) {
  if (V.page != g_painted) {
    board_lcd_clear(C_BG);
    g_painted = V.page;
#ifdef PFM_RTOS
    taskENTER_CRITICAL();
#endif
    dirty_n = 0;
#ifdef PFM_RTOS
    taskEXIT_CRITICAL();
#endif
  }
  if (V.cal) { draw_cal(); return; }
  if (V.page == PG_VIS) studio_analysis_update();
  else if (V.page == PG_MIX) studio_vu_update();
  draw_status();
  switch (V.page) {
  case PG_SEQ: draw_seq(); break;
  case PG_SYN: draw_syn(); break;
  case PG_MIX: draw_mix(); break;
  case PG_VIS: draw_vis(); break;
  default:     draw_sys(); break;
  }
  draw_hint(g_app.uptime_ms);
  draw_tabs();
}

void ui_draw(void) { ui_snapshot(); draw_page(); }

uint32_t ui_generation(void) { return V.gen; }

void ui_draw_dirty(void) {
  int16_t rect[UI_DIRTY_MAX][4];
  int n;
#ifdef PFM_RTOS
  taskENTER_CRITICAL();
#endif
  V = V_live;                 /* same snapshot as the dirty list */
  n = dirty_n;
  for (int i = 0; i < n; i++) {
    rect[i][0] = dirty_r[i][0]; rect[i][1] = dirty_r[i][1];
    rect[i][2] = dirty_r[i][2]; rect[i][3] = dirty_r[i][3];
  }
  dirty_n = 0;                /* app may add new dirty after this critical */
#ifdef PFM_RTOS
  taskEXIT_CRITICAL();
#endif
  if (!n) return;
  board_lcd_clip_reset();
  for (int i = 0; i < n; i++)
    board_lcd_clip_add(rect[i][0], rect[i][1], rect[i][2], rect[i][3]);
  draw_page();
  board_lcd_clip_reset();
}

/* Steady-state overlay: pages stay in GRAM, only these small regions are
   rewritten.  This is what removes the full-screen tear line. */
static void anim_body(void) {
  if (V.cal) return;
  uint32_t now = g_app.uptime_ms;
  static uint32_t last_cpu, last_hint;
  static int last_seq_step = -2, last_seq_play = -2, last_live = -2;

  if (now - last_cpu >= 500) {
    draw_status_cpu();
    last_cpu = now;
  }
  uint32_t hk = (now / 3500) | ((uint32_t)V.flash << 16) |
                ((uint32_t)(V.flash_until > now) << 31) |
                ((uint32_t)V.flash_val << 20) |
                ((uint32_t)V.mod << 14);
  if (hk != last_hint) {
    draw_hint(now);
    last_hint = hk;
  }

  switch (V.page) {
  case PG_SEQ: {
    int st = studio_step(), pl = studio_playing();
    if (st != last_seq_step || pl != last_seq_play) {
      draw_seq_ruler(st, pl);
      last_seq_step = st;
      last_seq_play = pl;
    }
    break;
  }
  case PG_SYN:
    draw_syn_tilt_anim();
    if (studio_live_note() != last_live) {
      draw_syn_keys_anim();
      last_live = studio_live_note();
    }
    break;
  case PG_MIX:
    studio_vu_update();
    draw_mix_vu_bars();
    break;
  case PG_VIS:
    studio_analysis_update();
    draw_vis_content();
    break;
  default:
    break;
  }
}

void ui_anim(void) { ui_snapshot(); anim_body(); }

/* ------------------------------------------------------------------------- */
/* page switching                                                            */
/* ------------------------------------------------------------------------- */
void ui_set_page(int page) {
  page = ((page % PG_N) + PG_N) % PG_N;
  if (page == U.page) return;
  if (U.page == PG_SYN) studio_live_focus(0);
  U.page = page;
  U.mod = 0;
  if (page == PG_SYN) {
    studio_live_focus(1);
    studio_live_patch(studio_live_patch_get());
  }
  studio_set_analysis(page == PG_VIS ? 2 : (page == PG_MIX ? 1 : 0));
  publish();
}

int ui_page(void) { return V_live.page; }

/* ------------------------------------------------------------------------- */
/* actions                                                                   */
/* ------------------------------------------------------------------------- */
/* Modal three-point touch calibration (SYS -> Touch Cal -> C).
   The targets are CircleOS' Primer2 reference points; ui_touch() collects the
   raw ADC points and board_touch_calib_set() computes A..F/R and writes BKP. */
static void ui_cal_start(uint32_t now) {
  if (U.cal) return;
  U.cal = 1;
  U.cal_step = 0;
  U.cal_msg = 0;
  U.cal_since = now;
  U.mod = 0;
  publish();
  ui_invalidate(0, 0, W, H);
}

static void ui_cal_end(void) {
  U.cal = 0;
  U.cal_step = 0;
  U.cal_msg = 0;
  publish();
  ui_invalidate(0, 0, W, H);
}

static void inv_seq_cell(int step, int track) {
  if (step < 0 || step >= ST_STEPS || track < 0 || track >= ST_TRACKS) return;
  ui_invalidate(26 + step * 6 - 1, 42 + track * 8 - 1, 8, 10);
}
static void inv_seq_row(int track) {
  if (track < 0 || track >= ST_TRACKS) return;
  ui_invalidate(0, 42 + track * 8, 26, 8);
}
static void inv_seq_footer(void) { ui_invalidate(0, 124, W, 13); }
static void inv_mix_row(int sel) {
  if (sel < ST_TRACKS) ui_invalidate(0, 28 + sel * 9, 96, 9);
  else ui_invalidate(0, 118, 96, 9);
}
static void inv_sys_row(int sel) { ui_invalidate(0, 28 + sel * 8, W, 8); }
static void syn_key_geom(int key, int *x, int *w) {
  static const uint8_t wb[12] = { 0, 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6 };
  static const uint8_t whites[7] = { 0, 2, 4, 5, 7, 9, 11 };
  int oct2 = key / 12, semi = key % 12;
  if (semi == 1 || semi == 3 || semi == 6 || semi == 8 || semi == 10) {
    *x = 1 + (oct2 * 7 + wb[semi] + 1) * 9 - 3; *w = 5;
  } else {
    int ww = 0; while (ww < 7 && whites[ww] != semi) ww++;
    *x = 1 + (oct2 * 7 + ww) * 9; *w = 8;
  }
}
static void inv_syn_key(int key, int oct) {
  int x, w;
  syn_key_geom(key, &x, &w);
  (void)oct;
  ui_invalidate(x - 2, 99, w + 4, 38);
  ui_invalidate(0, 129, W, 9);
}

static void page_nav(int e, uint32_t now) {
  (void)now;
  int ostep = U.seq_step, otrack = U.seq_track;
  int okey = U.synth_key, ooct = U.synth_oct;
  int omix = U.mix_sel, osys = U.sys_sel;
  int page = U.page;
  switch (page) {
  case PG_SEQ:
    if (e & BTN_UP) U.seq_track = (U.seq_track + ST_TRACKS - 1) % ST_TRACKS;
    if (e & BTN_DOWN) U.seq_track = (U.seq_track + 1) % ST_TRACKS;
    if (e & BTN_LEFT) U.seq_step = (U.seq_step + ST_STEPS - 1) % ST_STEPS;
    if (e & BTN_RIGHT) U.seq_step = (U.seq_step + 1) % ST_STEPS;
    break;
  case PG_SYN:
    if (e & BTN_LEFT) U.synth_key = (U.synth_key + 23) % 24;
    if (e & BTN_RIGHT) U.synth_key = (U.synth_key + 1) % 24;
    if (e & BTN_UP) U.synth_oct = clampi(U.synth_oct + 1, 2, 6);
    if (e & BTN_DOWN) U.synth_oct = clampi(U.synth_oct - 1, 2, 6);
    break;
  case PG_MIX:
    if (e & BTN_UP) U.mix_sel = (U.mix_sel + 10) % 11;
    if (e & BTN_DOWN) U.mix_sel = (U.mix_sel + 1) % 11;
    if (e & BTN_LEFT) {
      if (U.mix_sel < ST_TRACKS) {
        uint8_t *lv = &studio_song()->level[U.mix_sel];
        if (*lv > 0) (*lv)--;
        studio_sync_song();
      } else app_set_volume(clampi(g_app.volume - 1, 0, 10));
    }
    if (e & BTN_RIGHT) {
      if (U.mix_sel < ST_TRACKS) {
        uint8_t *lv = &studio_song()->level[U.mix_sel];
        if (*lv < 15) (*lv)++;
        studio_sync_song();
      } else app_set_volume(clampi(g_app.volume + 1, 0, 10));
    }
    break;
  case PG_VIS:
    break;
  default:
    if (e & BTN_UP) U.sys_sel = (U.sys_sel + SYS_N - 1) % SYS_N;
    if (e & BTN_DOWN) U.sys_sel = (U.sys_sel + 1) % SYS_N;
    if (e & (BTN_LEFT | BTN_RIGHT)) {
      int d = (e & BTN_RIGHT) ? 1 : -1;
      switch (U.sys_sel) {
      case SYS_VOL: app_set_volume(clampi(g_app.volume + d, 0, 10)); break;
      case SYS_BL: app_set_backlight(clampi(g_app.backlight + d, 0, 7)); break;
      case SYS_BPM: studio_set_bpm(clampi(studio_bpm() + d, 60, 200)); break;
      case SYS_SWING: studio_set_swing(clampi(studio_swing() + d, 0, 50)); break;
      case SYS_PAT: studio_select_pattern((studio_pattern() + d + 4) % 4); break;
      default: break;
      }
    }
    break;
  }
  switch (page) {
  case PG_SEQ:
    if (ostep != U.seq_step || otrack != U.seq_track) {
      inv_seq_cell(ostep, otrack);
      inv_seq_cell(U.seq_step, U.seq_track);
      inv_seq_footer();
      if (otrack != U.seq_track) { inv_seq_row(otrack); inv_seq_row(U.seq_track); }
    }
    break;
  case PG_SYN:
    if (okey != U.synth_key || ooct != U.synth_oct) {
      if (ooct != U.synth_oct) {
        ui_invalidate(0, 99, W, 38);
        ui_invalidate(0, 9, W, 17);
      } else {
        inv_syn_key(okey, ooct);
        inv_syn_key(U.synth_key, U.synth_oct);
      }
      ui_invalidate(0, 129, W, 9);
    }
    break;
  case PG_MIX:
    if (omix != U.mix_sel) { inv_mix_row(omix); inv_mix_row(U.mix_sel); }
    if (e & (BTN_LEFT | BTN_RIGHT)) {
      inv_mix_row(U.mix_sel);
      if (U.mix_sel == 10) ui_invalidate(0, 9, W, 17);
    }
    break;
  default:
    if (osys != U.sys_sel) { inv_sys_row(osys); inv_sys_row(U.sys_sel); }
    if (e & (BTN_LEFT | BTN_RIGHT)) {
      inv_sys_row(U.sys_sel);
      if (U.sys_sel == SYS_OUT) ui_invalidate(0, 9, W, 17);
    }
    break;
  }
  publish();
}

static void page_tap(uint32_t now) {
  switch (U.page) {
  case PG_SEQ: seq_toggle(); break;
  case PG_SYN: break;   /* note handled by press/release */
  case PG_MIX:
    if (U.mix_sel < ST_TRACKS) {
      studio_song()->mute[U.mix_sel] ^= 1;
      studio_sync_song();
    } else app_set_output(!g_app.output);
    break;
  case PG_VIS: U.vis_mode ^= 1; break;
  default:
    switch (U.sys_sel) {
    case SYS_OUT: app_set_output(!g_app.output); break;
    case SYS_CHAIN: studio_set_chain(!studio_chain()); break;
    case SYS_SAVE: {
      int r = app_save();
      flash_set(r > 0 ? 3 : 4, 0, now);
      break;
    }
    case SYS_LOAD:
      if (app_load() > 0) flash_set(3, 0, now);
      else flash_set(4, 0, now);
      break;
    case SYS_DEMO: app_demo(); break;
    case SYS_TOUCH: ui_cal_start(now); break;
    case SYS_SHUTDOWN: app_shutdown(); break;
    default: break;
    }
    break;
  }
  switch (U.page) {
  case PG_SEQ:
    inv_seq_cell(U.seq_step, U.seq_track);
    inv_seq_footer();
    break;
  case PG_MIX:
    inv_mix_row(U.mix_sel);
    if (U.mix_sel == 10) ui_invalidate(0, 9, W, 17);
    break;
  case PG_VIS:
    ui_invalidate(0, 9, W, 17);
    ui_invalidate(0, 28, W, 109);
    break;
  default:
    inv_sys_row(U.sys_sel);
    if (U.sys_sel == SYS_OUT) ui_invalidate(0, 9, W, 17);
    if (U.sys_sel == SYS_SAVE || U.sys_sel == SYS_LOAD) ui_invalidate(0, 137, W, 11);
    if (U.sys_sel == SYS_LOAD || U.sys_sel == SYS_DEMO) {
      ui_invalidate(0, 0, W, 8);
      ui_invalidate(0, 9, W, 17);
      ui_invalidate(0, 28, W, 109);
    }
    break;
  }
  publish();
}

static void page_double(uint32_t now) {
  (void)now;
  studio_play(!studio_playing());
  ui_invalidate(0, 0, W, 8);
  ui_invalidate(0, 137, W, 11);
  if (U.page == PG_SEQ) ui_invalidate(26, 38, 96, 3);
  publish();
}

static void page_mod(int e, uint32_t now) {
  switch (U.page) {
  case PG_SEQ:
    if (e & BTN_UP) seq_transpose(+1);
    if (e & BTN_DOWN) seq_transpose(-1);
    if (e & BTN_RIGHT) seq_octave(+1);
    if (e & BTN_LEFT) seq_octave(-1);
    inv_seq_cell(U.seq_step, U.seq_track);
    inv_seq_footer();
    break;
  case PG_SYN:
    if (e & (BTN_UP | BTN_DOWN)) {
      synth_patch_shift((e & BTN_UP) ? +1 : -1);
      ui_invalidate(0, 28, 100, 68);
      ui_invalidate(0, 9, W, 17);
      ui_invalidate(0, 129, W, 9);
    } else {
      if (e & BTN_LEFT) studio_live_arp(!studio_live_arp_get());
      if (e & BTN_RIGHT) studio_live_latch(!studio_live_latch_get());
      ui_invalidate(100, 66, 27, 11);
      ui_invalidate(100, 79, 27, 11);
      ui_invalidate(0, 129, W, 9);
    }
    break;
  default: break;
  }
  (void)now;
  publish();
}

/* ------------------------------------------------------------------------- */
/* input                                                                     */
/* ------------------------------------------------------------------------- */
static int prev_btn;
static uint32_t c_down, c_last, last_press;
static int c_taps, c_long;
static int held_dirs, held_used, held_lock;
static uint32_t held_since, held_repeat;

void ui_input(int b, uint32_t now) {
  int press = b & ~prev_btn;
  if (press && (uint32_t)(now - last_press) < 14) press = 0;
  if (press) last_press = now;

  int c_now = (b & BTN_CENTER) != 0;
  int c_prev = (prev_btn & BTN_CENTER) != 0;

  if (U.cal) {
    if (U.cal == 4 && (uint32_t)(now - U.cal_since) >= 1500) {
      ui_cal_end();
    } else if (c_now && !c_prev) {
      ui_cal_end();
    }
    if (!c_now && c_prev && U.page == PG_SYN) studio_live_key(-1, 0);
    c_long = 0; c_taps = 0;
    prev_btn = b;
    return;
  }

  /* ---- centre: single / double / triple / long ----
     The 5-way is one stick, so C+direction is replaced by:
       single C  = page action (after a 250 ms settle)
       double C  = play/stop
       triple C  = MOD layer; the next direction does the fine edit
       long C    = next page
     The settle delay is what lets the triple/double be recognised without
     the first tap already toggling a note. */
  if (c_now && !c_prev) {
    c_down = now;
    c_long = 0;
    if (U.page == PG_SYN) studio_live_key(synth_note(), 12);
  }
  if (c_now) {
    if (!c_long && (uint32_t)(now - c_down) >= 550) {
      c_long = 1;
      c_taps = 0;
      U.mod = 0;
      ui_set_page((U.page + 1) % PG_N);
    }
  } else if (c_prev) {
    if (U.page == PG_SYN) studio_live_key(-1, 0);
    if (c_long) {
      c_long = 0;
      c_taps = 0;
    } else {
      if (c_taps && (uint32_t)(now - c_last) <= 250) c_taps++;
      else c_taps = 1;
      c_last = now;
      if (c_taps >= 3) {
        c_taps = 0;
        if (U.page == PG_SEQ || U.page == PG_SYN) {
          U.mod = !U.mod;
          publish();
          ui_invalidate(0, 137, W, 11);
        }
      }
    }
  }

  /* Single/double settle: only fire while C is up, so a second press pauses
     the timer and can still become a triple. */
  if (c_taps && !c_now && (uint32_t)(now - c_last) >= 250) {
    int taps = c_taps;
    c_taps = 0;
    if (taps == 1) {
      page_tap(now);
    } else if (taps == 2) {
      page_double(now);
    }
  }

  /* ---- direction: MOD edit (one-shot), otherwise navigation ---- */
  int nav = press & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
  if (c_now) nav = 0;
  if (nav && U.mod) {
    page_mod(nav, now);
    ui_invalidate(0, 137, W, 11);
    held_dirs = nav;
    held_since = now;
    held_used = 1;
    held_lock = 1;
    held_repeat = now;
    nav = 0;
  }
  if (nav) page_nav(nav, now);

  /* ---- hold U/D (SEQ: pattern) / hold L/R (SEQ: tempo) ---- */
  int dirs = b & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
  if (dirs != held_dirs) {
    held_dirs = dirs;
    held_since = now;
    held_used = 0;
    held_lock = 0;
    held_repeat = now;
  }
  if (dirs && !c_now && !held_lock && U.page == PG_SEQ && !held_used &&
      (uint32_t)(now - held_since) >= 600) {
    held_used = 1;
    held_repeat = now;
    if (dirs == BTN_UP || dirs == BTN_DOWN) {
      int d = (dirs == BTN_DOWN) ? 1 : -1;
      studio_select_pattern((studio_pattern() + d + 4) % 4);
      flash_set(1, studio_pattern(), now);
    } else {
      int d = (dirs == BTN_RIGHT) ? 1 : -1;
      studio_set_bpm(clampi(studio_bpm() + d, 60, 200));
      flash_set(2, studio_bpm(), now);
      ui_invalidate(0, 9, W, 17);
    }
    publish();
  } else if (held_used && dirs && !held_lock && (uint32_t)(now - held_repeat) >= 160) {
    held_repeat = now;
    if (dirs == BTN_UP || dirs == BTN_DOWN) {
      int d = (dirs == BTN_DOWN) ? 1 : -1;
      studio_select_pattern((studio_pattern() + d + 4) % 4);
      flash_set(1, studio_pattern(), now);
    } else if (dirs == BTN_LEFT || dirs == BTN_RIGHT) {
      int d = (dirs == BTN_RIGHT) ? 1 : -1;
      studio_set_bpm(clampi(studio_bpm() + d, 60, 200));
      flash_set(2, studio_bpm(), now);
      ui_invalidate(0, 9, W, 17);
    }
    publish();
  }

  prev_btn = b;
}

/* ------------------------------------------------------------------------- */
/* touch                                                                     */
/* ------------------------------------------------------------------------- */
void ui_touch(int x, int y) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  if (U.cal) {
    if (U.cal == 4) return;
    int rx, ry;
    /* Use the uncalibrated raw point, not the matrix output. */
    if (!board_touch_raw(&rx, &ry)) return;
    int s = U.cal_step;
    if (s >= 3) return;
    U.cal_raw[2 * s] = rx;
    U.cal_raw[2 * s + 1] = ry;
    U.cal_step++;
    if (s == 2) {
      int ok = board_touch_calib_set(U.cal_raw);
      U.cal = 4;
      U.cal_msg = ok ? 1 : 2;
      U.cal_since = g_app.uptime_ms;
    }
    publish();
    ui_invalidate(0, 0, W, H);
    return;
  }
  if (y >= 148) {                       /* bottom tabs */
    int p = x / 25;
    if (p >= PG_N) p = PG_N - 1;
    ui_set_page(p);
    return;
  }
  if (y < 5) {                          /* four top areas / buttons */
    /* CircleOS buttons are the bottom 4x32 zones; this UI is rotated 180.
       Only the top 5 px strip is the button area, so taps on the header
       (y=5..25) do not change page by accident. */
    static const int top_pg[4] = { PG_SEQ, PG_SYN, PG_MIX, PG_VIS };
    int b = x / 32;
    if (b < 4) ui_set_page(top_pg[b]);
    return;
  }
  int page = U.page;
  switch (page) {
  case PG_SEQ: {
    int ot = U.seq_track, os = U.seq_step;
    if (y >= 38 && y < 42) {
      if (x >= 26 && x < 122) U.seq_step = clampi((x - 26) / 6, 0, ST_STEPS - 1);
      else return;
    } else if (y >= 42 && y < 122) {
      if (x < 26) {
        U.seq_track = clampi((y - 42) / 8, 0, ST_TRACKS - 1);
      } else if (x < 122) {
        U.seq_track = clampi((y - 42) / 8, 0, ST_TRACKS - 1);
        U.seq_step = clampi((x - 26) / 6, 0, ST_STEPS - 1);
      } else return;
    } else return;
    if (U.seq_track != ot || U.seq_step != os) {
      inv_seq_cell(os, ot);
      inv_seq_cell(U.seq_step, U.seq_track);
      inv_seq_footer();
      if (ot != U.seq_track) { inv_seq_row(ot); inv_seq_row(U.seq_track); }
      publish();
    }
    break;
  }
  case PG_SYN: {
    int pi = studio_live_patch_get();
    /* 8-patch grid */
    if (y >= 28 && y < 96 && ((x >= 2 && x < 48) || (x >= 50 && x < 96))) {
      int col = (x >= 50);
      int row = (y - 28) / 17;
      if (row >= 0 && row < 4) {
        int i = row * 2 + col;
        if (i < ST_PATCHES && i != pi) {
          studio_live_patch(i);
          publish();
          ui_invalidate(0, 28, 100, 68);
          ui_invalidate(0, 9, W, 17);
          ui_invalidate(0, 129, W, 9);
        }
      }
      break;
    }
    /* ARP / LATCH badges */
    if (x >= 100 && x < 127 && y >= 66 && y < 77) {
      studio_live_arp(!studio_live_arp_get());
      publish();
      ui_invalidate(100, 66, 27, 11);
      ui_invalidate(0, 129, W, 9);
      break;
    }
    if (x >= 100 && x < 127 && y >= 79 && y < 90) {
      studio_live_latch(!studio_live_latch_get());
      publish();
      ui_invalidate(100, 79, 27, 11);
      ui_invalidate(0, 129, W, 9);
      break;
    }
    /* two-octave keyboard: base = (oct+1)*12, key 0..23 */
    if (y >= 99 && y < 128) {
      static const uint8_t blacks[5] = { 1, 3, 6, 8, 10 };
      int hit = -1;
      if (y < 117) {
        for (int k = 0; k < 24 && hit < 0; k++) {
          int semi = k % 12;
          int isb = 0;
          for (int b = 0; b < 5; b++) if (blacks[b] == semi) isb = 1;
          if (!isb) continue;
          int kx, kw; syn_key_geom(k, &kx, &kw);
          if (x >= kx - 1 && x < kx + kw + 1) hit = k;
        }
      }
      for (int k = 0; k < 24 && hit < 0; k++) {
        int semi = k % 12;
        int isb = 0;
        for (int b = 0; b < 5; b++) if (blacks[b] == semi) isb = 1;
        if (isb) continue;
        int kx, kw; syn_key_geom(k, &kx, &kw);
        if (x >= kx && x < kx + kw) hit = k;
      }
      if (hit >= 0 && hit != U.synth_key) {
        int old = U.synth_key;
        U.synth_key = hit;
        publish();
        inv_syn_key(old, U.synth_oct);
        inv_syn_key(hit, U.synth_oct);
        ui_invalidate(0, 129, W, 9);
      }
    }
    break;
  }
  case PG_MIX: {
    if (x >= 96 || y < 28 || y >= 127) return;
    int old = U.mix_sel;
    int i;
    if (y >= 118) i = 10;
    else { i = (y - 28) / 9; if (i > 9) return; }
    if (i != old) {
      U.mix_sel = i;
      publish();
      inv_mix_row(old);
      inv_mix_row(i);
    }
    break;
  }
  case PG_SYS: {
    if (y < 28 || y >= 132) return;
    int old = U.sys_sel;
    int i = (y - 28) / 8;
    if (i < SYS_N && i != old) {
      U.sys_sel = i;
      publish();
      inv_sys_row(old);
      inv_sys_row(i);
    }
    break;
  }
  case PG_VIS:
    U.vis_mode ^= 1;
    publish();
    ui_invalidate(0, 9, W, 17);
    ui_invalidate(0, 28, W, 109);
    break;
  default: break;
  }
}

/* ------------------------------------------------------------------------- */
/* init / demo                                                               */
/* ------------------------------------------------------------------------- */
void ui_init(void) {
  memset(&U, 0, sizeof(U));
  memcpy(pencil, def_pencil, sizeof(pencil));
  U.page = PG_SEQ;
  U.synth_oct = 4;
  U.synth_key = 9;      /* A4 */
  U.vis_mode = 0;
  studio_set_analysis(0);
  publish();
}

/* Scripted showcase for the QEMU build (no joystick needed). */
void ui_sim_demo(uint32_t ms) {
  if (ms < 6500) {
    if (V.page != PG_SEQ) ui_set_page(PG_SEQ);
    U.seq_step = (int)((ms / 520) % 16);
    U.seq_track = (int)((ms / 2100) % 10);
    publish();
  } else if (ms < 13000) {
    static int last_key = -1;
    if (V.page != PG_SYN) {
      ui_set_page(PG_SYN);
      studio_live_arp(1);
      studio_live_latch(1);
      studio_live_patch(1);
      last_key = -1;
    }
    int key = (int)((ms / 650) % 24);
    if (key != last_key) {
      last_key = key;
      U.synth_key = key;
      studio_live_key((U.synth_oct + 1) * 12 + key, 12);
    }
    int tx = (int)((ms / 50) % 200) - 100;
    studio_live_tilt(tx, (int)((ms / 70) % 200) - 100);
    publish();
  } else if (ms < 19000) {
    if (V.page != PG_VIS) ui_set_page(PG_VIS);
    U.vis_mode = (ms >= 16000 && ms < 17600) ? 1 : 0;
    publish();
  } else if (ms < 21500) {
    if (V.page != PG_MIX) {
      ui_set_page(PG_MIX);
      studio_song()->mute[ST_TRK_FM3] = 1;
      studio_sync_song();
    }
    U.mix_sel = (int)((ms / 900) % 11);
    publish();
  } else {
    static int saved;
    if (V.page != PG_SYS) {
      ui_set_page(PG_SYS);
      studio_song()->mute[ST_TRK_FM3] = 0;
      studio_sync_song();
    }
    U.sys_sel = (int)((ms / 700) % SYS_N);
    if (!saved && ms > 22200) {
      saved = 1;
      int r = app_save();
      flash_set(r > 0 ? 3 : 4, 0, ms);
    }
    publish();
  }
}
