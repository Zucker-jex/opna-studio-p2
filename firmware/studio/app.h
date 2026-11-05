#ifndef STUDIO_APP_H
#define STUDIO_APP_H

#include <stdint.h>

/* UI-facing application state (volume/output/SD/statistics). */
struct app_state {
  volatile int volume;      /* 0..10 */
  volatile int backlight;   /* 0..7 */
  volatile int output;      /* 0 headphones, 1 speaker */
  volatile int have_sd;
  volatile int mems_ok;
  volatile int16_t mems_x, mems_y, mems_z;
  volatile int save_status; /* 0 idle, 1 ok, -1 error */
  volatile uint32_t cpu_pct;
  volatile uint32_t fm_pct;
  volatile uint32_t ssg_pct;
  volatile uint32_t drum_pct;
  volatile uint32_t fps;
  volatile uint32_t underruns;
  volatile uint32_t uptime_ms;
};

extern struct app_state g_app;

void app_set_volume(int v);
void app_set_backlight(int v);
void app_set_output(int v);
int  app_save(void);        /* pattern + settings -> /opna2.p2s */
int  app_load(void);        /* -> 1 ok, 0 no file, -1 error */
void app_demo(void);
void app_shutdown(void);

void studio_app_hw(void);
void studio_app_sim(void);

#endif
