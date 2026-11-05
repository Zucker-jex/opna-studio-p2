#ifndef STUDIO_H
#define STUDIO_H

/*
 * OPNA Studio P2 — engine interface.
 *
 * A 10-track, sample-accurate step sequencer + live performance layer driving
 * the same bit-exact YM2608 (OPNA) core as pocket-fmdsp, plus the analysis
 * buffers used by the visualizer.  No floating point, no malloc.
 *
 * Tracks:  0..5  FM1..FM6      (OPNA channels 1-6, monophonic per track)
 *          6..7  SSG1, SSG2    (tone channels A/B)
 *          8     NOISE         (SSG channel C in noise mode -> hats/percussion)
 *          9     DRM           (one of the six OPNA rhythm voices per step)
 */

#include <stdint.h>
#include "pfm/pfm_config.h"

#define ST_STEPS 16
#define ST_PATTERNS 4
#define ST_TRACKS 10

enum {
  ST_TRK_FM1 = 0, ST_TRK_FM2, ST_TRK_FM3, ST_TRK_FM4, ST_TRK_FM5, ST_TRK_FM6,
  ST_TRK_SSG1, ST_TRK_SSG2, ST_TRK_NOISE, ST_TRK_DRUM
};

/* One step: note 0..127 for melodic tracks, 0..5 drum id for DRM, 0xff = rest.
   gate 0..15 = note length as a fraction of the step (15 = full step). */
struct st_step {
  uint8_t note, vel, gate;
};

struct st_song {
  struct st_step step[ST_PATTERNS][ST_TRACKS][ST_STEPS];
  uint8_t mute[ST_TRACKS];   /* 1 = silent */
  uint8_t level[ST_TRACKS];  /* 0..15 mixer level */
  uint8_t patch[6];          /* FM1..FM6 patch index */
};

extern const char *const st_track_name[ST_TRACKS];
extern const char *const st_drum_name[6];

/* ---- lifecycle ---- */
void studio_init(void);            /* reset chip + engine; no demo content */
void studio_demo_song(void);       /* fill the song with the built-in demo */
void studio_clear_song(void);      /* silence + empty patterns */
struct st_song *studio_song(void);

/* Re-apply patch/level/mute state after the UI (or a file load) edited `song`. */
void studio_sync_song(void);

/* ---- transport ---- */
void studio_play(int on);
int studio_playing(void);
void studio_select_pattern(int p);
int studio_pattern(void);          /* pattern currently being sequenced */
int studio_step(void);             /* step currently playing, 0..15 */
int studio_bpm(void);
void studio_set_bpm(int bpm);      /* 60..200 */
int studio_swing(void);
void studio_set_swing(int pct);    /* 0..50 */
int studio_chain(void);
void studio_set_chain(int on);
uint32_t studio_frames(void);      /* total rendered frames (free-running) */

/* ---- live performance layer (FM6) ---- */
void studio_live_focus(int on);    /* 1 while the SYNTH page owns FM6 */
int  studio_live_focus_get(void);
void studio_live_key(int note, int vel); /* note-on; note<0 = key up */
void studio_live_off(void);
void studio_live_patch(int patch);
int  studio_live_patch_get(void);
void studio_live_arp(int on);
int  studio_live_arp_get(void);
void studio_live_latch(int on);
int  studio_live_latch_get(void);
int  studio_live_note(void);       /* note currently sounding in the live voice */
int  studio_live_active(void);
void studio_live_tilt(int x, int y); /* -1000..1000; pitch bend on live voice */
int  studio_live_tilt_x(void);     /* -100..100 (filtered) */
int  studio_live_tilt_y(void);

/* ---- note helpers used by the UI ---- */
void studio_preview(int track, int note); /* short audition on that track */

/* ---- audio rendering ---- */
/* Renders interleaved stereo int16 frames, processing sequencer events at
   sample accuracy.  Also feeds the scope/spectrum capture ring. */
void studio_render(int16_t *buf, int frames);

/* True when nothing needs synthesis (stopped, no live/preview/tail): the app
   can feed silence without paying the OPNA cost. */
int studio_is_idle(void);

/* ---- visualizer analysis ----
   0 = off, 1 = VU only (MIX), 2 = FFT + scope + VU (VIS).  The page switch
   calls this so the per-sample capture and FFT run only when visible. */
void studio_set_analysis(int mode);
void studio_analysis_update(void);
void studio_vu_update(void);
const uint8_t *studio_spectrum(void);      /* 64 bins, 0..63 */
const uint8_t *studio_spectrum_peak(void); /* 64 peak-hold values */
const int16_t *studio_scope(void);         /* 128 mono samples */
int studio_level(int ch);                  /* 0..100 VU */
uint32_t studio_signal_frames(void);       /* frames of audio captured */

#endif /* STUDIO_H */
