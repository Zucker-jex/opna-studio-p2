#include "app.h"
#include "ui.h"
#include "studio.h"
#include "board.h"
#include "pfm/pfm_prof.h"

#include <string.h>
#include <stddef.h>

#ifdef PFM_RTOS
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include "pwr_support.h"
#endif
#ifdef PFM_SIM
#include "semihosting.h"
#endif

struct app_state g_app;

#define ST_FILE_MAGIC "OPN2"
#define ST_FILE_VER 2

struct st_file {
  char magic[4];
  uint8_t ver;
  uint8_t bpm, swing, chain, pat;
  uint8_t vol, bl, out, reserved;
  uint16_t song_len;
  struct st_song song;
  uint16_t sum;
} __attribute__((packed));

static struct st_file g_file;
static uint16_t file_sum(const struct st_file *f) {
  const uint8_t *p = (const uint8_t *)f;
  size_t n = offsetof(struct st_file, sum);
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += p[i];
  return (uint16_t)s;
}

void app_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  g_app.volume = v;
  board_audio_set_volume(v);
}
void app_set_backlight(int v) {
  if (v < 0) v = 0;
  if (v > BOARD_BL_MAX) v = BOARD_BL_MAX;
  g_app.backlight = v;
  board_lcd_backlight(v);
}
void app_set_output(int v) {
  g_app.output = v ? 1 : 0;
  board_audio_set_output(g_app.output);
}

int app_save(void) {
  memset(&g_file, 0, sizeof(g_file));
  memcpy(g_file.magic, ST_FILE_MAGIC, 4);
  g_file.ver = ST_FILE_VER;
  g_file.bpm = (uint8_t)studio_bpm();
  g_file.swing = (uint8_t)studio_swing();
  g_file.chain = (uint8_t)(studio_chain() ? 1 : 0);
  g_file.pat = (uint8_t)studio_pattern();
  g_file.vol = (uint8_t)g_app.volume;
  g_file.bl = (uint8_t)g_app.backlight;
  g_file.out = (uint8_t)g_app.output;
  g_file.song_len = (uint16_t)sizeof(struct st_song);
  memcpy(&g_file.song, studio_song(), sizeof(struct st_song));
  g_file.sum = file_sum(&g_file);

  board_audio_mute(1);
  int r = board_file_save("/opna2.p2s", &g_file, (int)sizeof(g_file));
  board_audio_restart();
  board_audio_mute(0);
  g_app.save_status = (r == 0) ? 1 : -1;
  return (r == 0) ? 1 : 0;
}

int app_load(void) {
  int n = board_file_load("/opna2.p2s", &g_file, (int)sizeof(g_file));
  if (n <= 0) return 0;
  if (n != (int)sizeof(g_file)) return -1;
  if (memcmp(g_file.magic, ST_FILE_MAGIC, 4) != 0) return -1;
  if (g_file.ver != ST_FILE_VER && g_file.ver != 1) return -1;
  if (g_file.song_len != (uint16_t)sizeof(struct st_song)) return -1;
  if (g_file.sum != file_sum(&g_file)) return -1;

  int was_playing = studio_playing();
  studio_play(0);
  memcpy(studio_song(), &g_file.song, sizeof(struct st_song));
  if (g_file.ver == 1) {
    /* v1 是旧版“全 6 FM”存档：迁移时把 FM4-6 静音，避免一上电就回到 107%。
       用户仍可在 MIX 里逐轨重新打开。 */
    studio_song()->mute[ST_TRK_FM4] = 1;
    studio_song()->mute[ST_TRK_FM5] = 1;
    studio_song()->mute[ST_TRK_FM6] = 1;
  }
  studio_set_bpm(g_file.bpm);
  studio_set_swing(g_file.swing);
  studio_set_chain(g_file.chain);
  studio_select_pattern(g_file.pat);
  app_set_volume(g_file.vol);
  app_set_backlight(g_file.bl);
  app_set_output(g_file.out);
  studio_sync_song();
  if (was_playing) studio_play(1);
  g_app.save_status = 1;
  return 1;
}

void app_demo(void) {
  studio_play(0);
  studio_demo_song();
  studio_sync_song();
  studio_select_pattern(0);
  studio_play(1);
}

void app_shutdown(void) {
#ifdef PFM_RTOS
  pwr_shutdown();
#endif
}

/* ------------------------------------------------------------------------- */
/* statistics                                                                */
/* ------------------------------------------------------------------------- */
#ifdef PFM_RTOS
static uint64_t g_render_cycles;
static uint32_t g_render_frames;
static void stats_update(void) {
  uint64_t budget = (uint64_t)g_render_frames * board_cpu_hz() / PFM_MIX_RATE;
  uint32_t cpu = budget ? (uint32_t)(g_render_cycles * 100u / budget) : 0;
  if (cpu > 999) cpu = 999;
  g_app.cpu_pct = cpu;
  if (budget) {
    g_app.fm_pct = (uint32_t)((uint64_t)pfm_prof_cyc[PFM_PROF_FM] * 100u / budget);
    g_app.ssg_pct = (uint32_t)((uint64_t)pfm_prof_cyc[PFM_PROF_SSG] * 100u / budget);
    g_app.drum_pct = (uint32_t)((uint64_t)pfm_prof_cyc[PFM_PROF_DRUM] * 100u / budget);
  }
  for (int i = 0; i < PFM_PROF_N; i++) pfm_prof_cyc[i] = 0;
  g_app.underruns = board_audio_underruns();
  g_render_cycles = 0;
  g_render_frames = 0;
}
#endif

/* ------------------------------------------------------------------------- */
/* hardware application                                                      */
/* ------------------------------------------------------------------------- */
#ifdef PFM_RTOS
static FATFS g_fs;
static StaticTask_t s_app_tcb, s_lcd_tcb, s_idle_tcb;
static StackType_t s_app_stack[1024];
static StackType_t s_lcd_stack[640];
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE];
static int16_t s_chunk[256 * 2];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   uint32_t *size) {
  *tcb = &s_idle_tcb;
  *stack = s_idle_stack;
  *size = configMINIMAL_STACK_SIZE;
}

static void app_init(void) {
  int16_t mx, my, mz;
  g_app.volume = 8;
  g_app.backlight = 7;
  g_app.output = 0;
  g_app.mems_ok = (board_motion(&mx, &my, &mz) == 0);

  studio_init();
  studio_demo_song();
  g_app.have_sd = (f_mount(&g_fs, "", 1) == FR_OK);
  board_audio_open(PFM_MIX_RATE, 0);
  if (g_app.have_sd) {
    if (app_load() <= 0) {
      studio_demo_song();
      studio_sync_song();
    }
  } else {
    studio_sync_song();
  }
  app_set_volume(g_app.volume);
  app_set_backlight(g_app.backlight);
  app_set_output(g_app.output);
  studio_select_pattern(0);
  studio_play(1);
}

static void app_task(void *arg) {
  (void)arg;
  TickType_t last_stats = xTaskGetTickCount();
  TickType_t last_mems = last_stats;
  TickType_t last_delay = last_stats;
  for (;;) {
    TickType_t now = xTaskGetTickCount();
    uint32_t ms = (uint32_t)now;
    g_app.uptime_ms = ms;
    if (board_audio_ring_fill() < 512) {
      if (studio_is_idle()) {
        /* Stopped: feed silence and skip the whole OPNA render.  This is what
           makes pause drop from ~57% to ~0% on the CPU meter. */
        memset(s_chunk, 0, sizeof(s_chunk));
        board_audio_write(s_chunk, 256);
      } else {
        uint32_t t0 = board_cycles();
        studio_render(s_chunk, 256);
        g_render_cycles += board_cycles() - t0;
        g_render_frames += 256;
        board_audio_write(s_chunk, 256);
      }
    }
    if (now - last_stats >= 500) {
      stats_update();
      last_stats = now;
    }
    if (now - last_mems >= 40) {
      int16_t x, y, z;
      if (board_motion(&x, &y, &z) == 0) {
        studio_live_tilt(x, y);
        g_app.mems_x = x; g_app.mems_y = y; g_app.mems_z = z;
      }
      int tx, ty;
      int tp = board_touch_read(&tx, &ty);
      static int touch_last;
      if (tp && !touch_last) ui_touch(tx, ty);
      touch_last = tp;
      last_mems = now;
    }
    int b = board_input_poll();
    ui_input(b, ms);
    if (now - last_delay >= 4) {
      vTaskDelay(1);
      last_delay = now;
    }
  }
}

static void lcd_task(void *arg) {
  (void)arg;
  TickType_t last = xTaskGetTickCount();
  TickType_t sec = last;
  uint32_t draws = 0;
  int last_page = -1;
  int last_pat = -1;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(8));
    TickType_t now = xTaskGetTickCount();
    if (now - last >= 40 && board_audio_ring_fill() > 512) {
      int page = ui_page();
      int pat = studio_pattern();
      if (page != last_page) {
        ui_draw();                 /* page change: full, with clear */
        last_page = page;
        last_pat = pat;
      } else if (ui_dirty_pending()) {
        ui_draw_dirty();           /* input: only the changed cells/rows */
      } else if (page == PG_SEQ && pat != last_pat) {
        ui_redraw_pattern();       /* only the SEQ grid/header/footer */
        last_pat = pat;
      } else {
        ui_anim();                 /* playhead/VU/FFT/tilt only */
      }
      board_lcd_present();
      draws++;
      last = now;
    }
    if (now - sec >= 1000) {
      g_app.fps = draws;
      draws = 0;
      sec = now;
    }
  }
}

void studio_app_hw(void) {
  board_init();
  ui_init();
  ui_splash();
  uint32_t t0 = board_cycles();
  while ((uint32_t)(board_cycles() - t0) < board_cpu_hz()) {
  }
  app_init();
  TaskHandle_t app_h =
    xTaskCreateStatic(app_task, "app", 1024, NULL, 2, s_app_stack, &s_app_tcb);
  xTaskCreateStatic(lcd_task, "lcd", 640, NULL, 1, s_lcd_stack, &s_lcd_tcb);
  (void)app_h;
  vTaskStartScheduler();
  for (;;) {
  }
}
#endif /* PFM_RTOS */

/* ------------------------------------------------------------------------- */
/* QEMU / semihosting application (self-running showcase)                    */
/* ------------------------------------------------------------------------- */
#ifdef PFM_SIM
#define DEMO_SECONDS 24

void studio_app_sim(void) {
  board_init();
  ui_init();
  ui_splash();
  board_lcd_present();

  studio_init();
  studio_demo_song();
  g_app.volume = 8;
  g_app.backlight = 7;
  g_app.have_sd = 1;
  g_app.mems_ok = 1;
  board_audio_open(PFM_MIX_RATE, (uint32_t)DEMO_SECONDS * PFM_MIX_RATE);
  app_set_volume(g_app.volume);
  studio_play(1);
  ui_set_page(PG_SEQ);

  /* Optional scripted joystick for UI bring-up:
       sim_input.txt lines:  <ms> <mask>   (# = comment)
     mask: UP=1 DOWN=2 LEFT=4 RIGHT=8 CENTER=16.  Without the file the demo
     drives itself through the five pages. */
  static char script[1024];
  static uint16_t ev_ms[64];
  static uint8_t ev_mask[64];
  int ev_n = 0;
  {
    int n = board_file_load("/sim_input.txt", script, (int)sizeof(script) - 1);
    if (n > 0) {
      int i = 0;
      while (i < n && ev_n < 64) {
        while (i < n && (script[i] == '\n' || script[i] == '\r' || script[i] == ' ')) i++;
        if (i >= n || script[i] == '#') { while (i < n && script[i] != '\n') i++; continue; }
        int ms = 0, mask = 0;
        while (i < n && script[i] >= '0' && script[i] <= '9') { ms = ms * 10 + (script[i] - '0'); i++; }
        while (i < n && script[i] == ' ') i++;
        while (i < n && script[i] >= '0' && script[i] <= '9') { mask = mask * 10 + (script[i] - '0'); i++; }
        while (i < n && script[i] != '\n') i++;
        ev_ms[ev_n] = (uint16_t)ms;
        ev_mask[ev_n] = (uint8_t)mask;
        ev_n++;
      }
      sh_write0("[sim] scripted joystick loaded\n");
    }
  }
  int ev_i = 0;
  uint8_t cur_mask = 0;

  static int16_t chunk[256 * 2];
  const uint32_t total = (uint32_t)DEMO_SECONDS * PFM_MIX_RATE;
  uint32_t next_draw = 0, next_shot = 0;
  while (studio_frames() < total) {
    uint32_t left = total - studio_frames();
    unsigned n = left > 256 ? 256 : (unsigned)left;
    if (n == 0) break;
    studio_render(chunk, n);
    board_audio_write(chunk, n);
    uint32_t ms = (uint32_t)((uint64_t)studio_frames() * 1000u / PFM_MIX_RATE);
    g_app.uptime_ms = ms;
    g_app.cpu_pct = 41 + (ms / 1000) % 9;
    g_app.fm_pct = 24;
    g_app.ssg_pct = 6;
    g_app.drum_pct = 3;
    g_app.fps = 20;
    g_app.underruns = 0;
    if (ev_n > 0) {
      while (ev_i < ev_n && ms >= ev_ms[ev_i]) cur_mask = ev_mask[ev_i++];
      ui_input(cur_mask, ms);
    } else {
      ui_sim_demo(ms);
    }

    uint32_t f = studio_frames();
    if (next_draw == 0) next_draw = f + PFM_MIX_RATE / 20;
    if (ms < 1200) {
      /* keep the splash on screen for the intro */
      if (f >= next_shot) {
        ui_splash();
        board_lcd_present();
        next_shot = f + PFM_MIX_RATE / 20;
      }
    } else {
      if (f >= next_draw) {
        ui_draw();
        if (f >= next_shot) {
          board_lcd_present();
          next_shot = f + PFM_MIX_RATE / 20;
        }
        next_draw = f + PFM_MIX_RATE / 20;
      }
    }
  }
  board_audio_close();
  sh_write0("\n=== OPNA Studio demo complete ===\n");
  sh_exit(0);
}
#endif /* PFM_SIM */
