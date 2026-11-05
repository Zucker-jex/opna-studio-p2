/*
 * QEMU / semihosting board backend for OPNA Studio.
 *
 *   LCD     -> RGB565 framebuffer dumped as PPM screenshots on present()
 *   audio   -> streamed to sim_play_000.wav (header pre-sized for the demo)
 *   files   -> host files in QEMU's working directory (semihosting)
 *   input   -> a scripted sequence (optional; the demo drives itself)
 *   motion  -> a synthetic slow tilt
 */
#include "board.h"
#include "semihosting.h"

#include <string.h>

static uint16_t g_fb[BOARD_LCD_W * BOARD_LCD_H];
static int g_shot_no;

static inline int inb(int x, int y) {
  return (unsigned)x < BOARD_LCD_W && (unsigned)y < BOARD_LCD_H;
}

void board_lcd_clear(uint16_t color) {
  for (int i = 0; i < BOARD_LCD_W * BOARD_LCD_H; i++) g_fb[i] = color;
}

void board_lcd_pixel(int x, int y, uint16_t color) {
  if (inb(x, y)) g_fb[y * BOARD_LCD_W + x] = color;
}

void board_lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) board_lcd_pixel(x + i, y + j, color);
}

void board_lcd_blit(int x, int y, int w, int h, const uint16_t *pix) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      board_lcd_pixel(x + i, y + j, pix[j * w + i]);
}

static void num3(char *d, int n) {
  d[0] = (char)('0' + (n / 100) % 10);
  d[1] = (char)('0' + (n / 10) % 10);
  d[2] = (char)('0' + n % 10);
}

void board_lcd_backlight(int level) { (void)level; }

void board_touch_init(void) { }
int board_touch_read(int *x, int *y) { (void)x; (void)y; return 0; }
int board_touch_raw(int *x, int *y) { (void)x; (void)y; return 0; }
int board_touch_calib_set(const int raw[6]) { (void)raw; return 0; }
int board_touch_calib_points(int raw[6]) { (void)raw; return 0; }
int board_touch_calib_valid(void) { return 0; }

void board_lcd_clip_reset(void) { }
void board_lcd_clip_add(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void board_lcd_canvas_begin(uint16_t *buf, int x, int y, int w, int h) {
  (void)buf; (void)x; (void)y; (void)w; (void)h;
}
void board_lcd_canvas_end(void) { }

void board_lcd_present(void) {
  char path[] = "sim_shot_000.ppm";
  num3(path + 9, g_shot_no++);
  int fd = sh_open(path, 5 /* wb */);
  if (fd < 0) return;
  const char hdr[] = "P6\n128 160\n255\n";
  sh_write(fd, hdr, sizeof(hdr) - 1);
  uint8_t row[BOARD_LCD_W * 3];
  for (int y = 0; y < BOARD_LCD_H; y++) {
    for (int x = 0; x < BOARD_LCD_W; x++) {
      uint16_t p = g_fb[y * BOARD_LCD_W + x];
      row[x * 3 + 0] = (uint8_t)((p >> 11) << 3);
      row[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3f) << 2);
      row[x * 3 + 2] = (uint8_t)((p & 0x1f) << 3);
    }
    sh_write(fd, row, sizeof(row));
  }
  sh_close(fd);
}

/* ---------- optional scripted joystick ---------- */
static char g_input[256];
static int g_input_len, g_input_pos;

int board_input_wait(void) {
  while (g_input_pos < g_input_len) {
    char c = g_input[g_input_pos++];
    switch (c) {
    case 'U': case 'u': return BTN_UP;
    case 'D': case 'd': return BTN_DOWN;
    case 'L': case 'l': return BTN_LEFT;
    case 'R': case 'r': return BTN_RIGHT;
    case 'C': case 'c': return BTN_CENTER;
    default: break;
    }
  }
  return 0;
}

int board_input_poll(void) { return 0; }

uint32_t board_cycles(void) { static uint32_t c; return (c += 1000000u); }
uint32_t board_cpu_hz(void) { return 132000000u; }

/* ---------- audio ---------- */
static int g_wav_fd = -1;

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void board_audio_set_volume(int level) { (void)level; }
void board_audio_mute(int on) { (void)on; }
void board_audio_set_output(int speaker) { (void)speaker; }
void board_audio_restart(void) {}
uint32_t board_sd_read_errors(void) { return 0; }
uint32_t board_audio_underruns(void) { return 0; }
uint32_t board_audio_consumed_frames(void) { return 0; }
int32_t board_audio_ring_fill(void) { return 1 << 20; }

void board_audio_open(unsigned rate, uint32_t total_frames) {
  g_wav_fd = sh_open("sim_play_000.wav", 5 /* wb */);
  if (g_wav_fd < 0) return;
  uint32_t data = total_frames * 4;
  uint8_t h[44];
  memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2);
  put32(h + 24, rate); put32(h + 28, rate * 4); put16(h + 32, 4); put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, data);
  sh_write(g_wav_fd, h, 44);
  board_log("[sim] audio -> sim_play_000.wav\n");
}

void board_audio_write(const int16_t *stereo, size_t frames) {
  if (g_wav_fd >= 0) sh_write(g_wav_fd, stereo, frames * 4);
}

void board_audio_close(void) {
  if (g_wav_fd >= 0) sh_close(g_wav_fd);
  g_wav_fd = -1;
}

/* ---------- files ---------- */
static const char *strip_root(const char *name) {
  return (name && name[0] == '/') ? name + 1 : name;
}

int board_file_save(const char *name, const void *buf, int len) {
  int fd = sh_open(strip_root(name), 5 /* wb */);
  if (fd < 0) return -1;
  int n = sh_write(fd, buf, (size_t)len);
  sh_close(fd);
  return n == 0 ? 0 : -1;
}

int board_file_load(const char *name, void *buf, int maxlen) {
  int fd = sh_open(strip_root(name), 1 /* rb */);
  if (fd < 0) return -1;
  long len = sh_flen(fd);
  if (len < 0) { sh_close(fd); return -1; }
  if (len > maxlen) len = maxlen;
  int nr = sh_read(fd, buf, (size_t)len);
  sh_close(fd);
  if (nr < 0) return -1;
  return (int)(len - nr);
}

/* ---------- accelerometer (synthetic, slow sweep) ---------- */
int board_motion(int16_t *x, int16_t *y, int16_t *z) {
  static int t;
  int v = ((t += 7) % 400) - 200;
  if (x) *x = (int16_t)v;
  if (y) *y = (int16_t)(-v / 2);
  if (z) *z = 700;
  return 0;
}

/* ---------- lifecycle ---------- */
void board_log(const char *s) { sh_write0(s); }

void board_init(void) {
  board_log("[sim] OPNA Studio boot\n");
}
