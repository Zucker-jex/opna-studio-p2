#include "studio.h"
#include "patches.h"
#include "notes.h"
#include "fft.h"
#include "pfm/opna.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Engine state (declared first: the register helpers below use it)          */
/* ------------------------------------------------------------------------- */
struct engine {
  struct opna opna;
  struct st_song song;

  int bpm, swing, chain, playing, pattern, step, step_next;
  uint32_t pos, bar_start, next_step;
  uint32_t step_frames, swing_frames;

  uint8_t note_active[ST_TRACKS];
  uint32_t note_off_at[ST_TRACKS];

  /* live layer */
  int live_focus, live_held, live_latched, live_arp;
  int live_note, live_base, live_sounding, live_patch;
  uint32_t arp_at;
  int arp_idx, bend, tilt_x, tilt_y;
  uint32_t activity_until;   /* preview/drum activity even when stopped */

  /* ssg decay */
  uint8_t ssg_vol[3], ssg_decay[3];
  uint32_t ssg_next;

  /* analysis */
  int16_t mono[ST_FFT_N];
  unsigned mono_w;
  int32_t vu_hold[2];
  int level_pct[2];
  int analysis;   /* 0 off, 1 VU, 2 FFT+scope+VU */
};

static struct engine E;
static struct st_song *const S = &E.song;

const char *const st_track_name[ST_TRACKS] = {
  "FM1", "FM2", "FM3", "FM4", "FM5", "FM6",
  "SSG1", "SSG2", "NOI", "DRM",
};
const char *const st_drum_name[6] = { "BD", "SD", "TP", "HH", "TM", "RM" };

/* ------------------------------------------------------------------------- */
/* OPNA register helpers                                                     */
/* ------------------------------------------------------------------------- */
static void fm_write_patch(int ch, int patch) {
  const struct st_patch *p = &st_patches[patch % ST_PATCHES];
  unsigned b = ch >= 3 ? 0x100u : 0u, c = (unsigned)ch % 3u;
  for (int s = 0; s < 4; s++) {
    const struct st_op *o = &p->op[s];
    unsigned r = 0x30u + b + c + 4u * (unsigned)s;
    opna_writereg(&E.opna, r,        ((unsigned)o->det << 4) | (o->mul & 15));
    opna_writereg(&E.opna, r + 0x10, o->tl & 0x7f);
    opna_writereg(&E.opna, r + 0x20, ((unsigned)o->ks << 6) | (o->ar & 31));
    opna_writereg(&E.opna, r + 0x30, o->dr & 31);
    opna_writereg(&E.opna, r + 0x40, o->sr & 31);
    opna_writereg(&E.opna, r + 0x50, ((unsigned)o->sl << 4) | (o->rr & 15));
  }
  opna_writereg(&E.opna, 0xB0u + b + c, ((unsigned)p->fb << 3) | (p->alg & 7));
  opna_writereg(&E.opna, 0xB4u + b + c, 0xC0);  /* L+R, no LFO */
}

static void fm_key(int ch, int on) {
  unsigned b = ch >= 3 ? 4u : 0u, c = (unsigned)ch % 3u;
  opna_writereg(&E.opna, 0x28, (on ? 0xF0u : 0x00u) | b | c);
}

static void fm_freq(int ch, int note) {
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  unsigned b = ch >= 3 ? 0x100u : 0u, c = (unsigned)ch % 3u;
  unsigned blk = studio_note_blk[note], fnum = studio_note_fnum[note];
  opna_writereg(&E.opna, 0xA4u + b + c, (blk << 3) | (fnum >> 8));
  opna_writereg(&E.opna, 0xA0u + b + c, fnum & 0xff);
}

/* Base TLs of the current patch, and the velocity/level of active notes. */
static uint8_t g_base_tl[6][4];
static uint8_t g_active_vel[ST_TRACKS];
static int     g_active_patch[6];

static void fm_tl_update(int ch) {
  const struct st_patch *p = &st_patches[g_active_patch[ch] % ST_PATCHES];
  uint8_t carriers = st_patch_carriers(p->alg);
  unsigned b = ch >= 3 ? 0x100u : 0u, c = (unsigned)ch % 3u;
  int vel = g_active_vel[ch];
  int lvl = S->level[ch];
  int att = (15 - vel) * 2 + (15 - lvl) * 2;
  for (int s = 0; s < 4; s++) {
    int tl = g_base_tl[ch][s];
    if (carriers & (1u << s)) tl += att;
    if (tl > 127) tl = 127;
    opna_writereg(&E.opna, 0x40u + b + c + 4u * (unsigned)s, (unsigned)tl);
  }
}

static void fm_note_on(int ch, int note, int vel) {
  if (vel < 0) vel = 0;
  if (vel > 15) vel = 15;
  g_active_vel[ch] = (uint8_t)vel;
  g_active_patch[ch] = S->patch[ch];
  fm_key(ch, 0);
  fm_freq(ch, note);
  fm_tl_update(ch);
  fm_key(ch, 1);
}

static void fm_note_off(int ch) { fm_key(ch, 0); }

/* ------------------------------------------------------------------------- */
/* SSG / noise / drums                                                       */
/* ------------------------------------------------------------------------- */
static void ssg_note_on(int ch, int note, int vel, int level) {
  unsigned tp = studio_note_ssg[note < 0 ? 0 : (note > 127 ? 127 : note)];
  unsigned vol = (unsigned)((level * (vel + 1)) >> 4);
  if (vol > 15) vol = 15;
  if (vol == 0) vol = 1;
  opna_writereg(&E.opna, 0x00u + 2u * (unsigned)ch, tp & 0xff);
  opna_writereg(&E.opna, 0x01u + 2u * (unsigned)ch, (tp >> 8) & 0x0f);
  opna_writereg(&E.opna, 0x08u + (unsigned)ch, vol);
  E.ssg_vol[ch] = (uint8_t)vol;
  E.ssg_decay[ch] = 0;
}

static void ssg_off(int ch) {
  E.ssg_vol[ch] = 0;
  E.ssg_decay[ch] = 0;
  opna_writereg(&E.opna, 0x08u + (unsigned)ch, 0);
}

static void noise_hit(int vel, int level) {
  unsigned vol = (unsigned)((level * (vel + 1)) >> 4);
  if (vol > 15) vol = 15;
  if (vol == 0) vol = 1;
  opna_writereg(&E.opna, 0x0A, vol);
  E.ssg_vol[2] = (uint8_t)vol;
  E.ssg_decay[2] = 2;
}

/* YM2608 rhythm: register level 31 is loudest (the engine XORs it). */
static void drum_hit(int d, int vel) {
  if (d < 0) d = 0;
  if (d > 5) d = 5;
  int lvl = (vel * 31) / 15;
  if (lvl < 0) lvl = 0;
  if (lvl > 31) lvl = 31;
  opna_writereg(&E.opna, 0x18u + (unsigned)d, 0xC0u | (unsigned)lvl);
  opna_writereg(&E.opna, 0x10, 1u << (unsigned)d);
}

/* ------------------------------------------------------------------------- */
/* Song editing helpers                                                      */
/* ------------------------------------------------------------------------- */
static void put(int pat, int trk, int step, int note, int vel, int gate) {
  struct st_step *s = &S->step[pat][trk][step];
  s->note = (uint8_t)note;
  s->vel = (uint8_t)vel;
  s->gate = (uint8_t)gate;
}

static void clear_steps(void) {
  memset(S->step, 0xFF, sizeof(S->step));
}

static void seq16(int pat, int trk, const uint8_t *notes, int vel, int gate) {
  for (int s = 0; s < ST_STEPS; s++)
    if (notes[s] != 0xFF) put(pat, trk, s, notes[s], vel, gate);
}

static void arp16(int pat, int trk, const uint8_t *notes, int n, int vel, int gate) {
  for (int s = 0; s < ST_STEPS; s++) put(pat, trk, s, notes[s % n], vel, gate);
}

static void drums16(int pat, const uint8_t *ids, const uint8_t *vels) {
  for (int s = 0; s < ST_STEPS; s++)
    if (ids[s] != 0xFF) put(pat, ST_TRK_DRUM, s, ids[s], vels[s], 2);
}

/* ------------------------------------------------------------------------- */
/* Demo song: four one-bar patterns, A minor -> F -> C -> G                  */
/* ------------------------------------------------------------------------- */
static const uint8_t ARP_A[4] = { 57, 60, 64, 69 };
static const uint8_t ARP_B[4] = { 53, 57, 60, 65 };
static const uint8_t ARP_C[4] = { 60, 64, 67, 72 };
static const uint8_t ARP_D[4] = { 55, 59, 62, 67 };

static const uint8_t HI_A[4] = { 81, 84, 88, 93 };
static const uint8_t HI_B[4] = { 77, 81, 84, 89 };
static const uint8_t HI_C[4] = { 84, 88, 91, 96 };
static const uint8_t HI_D[4] = { 79, 83, 86, 91 };

static const uint8_t BASS_A[16] = { 45,0xff,0xff,45,0xff,0xff,57,0xff,45,0xff,0xff,43,0xff,0xff,40,0xff };
static const uint8_t BASS_B[16] = { 41,0xff,0xff,41,0xff,0xff,53,0xff,41,0xff,0xff,48,0xff,0xff,43,0xff };
static const uint8_t BASS_C[16] = { 48,0xff,0xff,48,0xff,0xff,60,0xff,48,0xff,0xff,55,0xff,0xff,43,0xff };
static const uint8_t BASS_D[16] = { 43,0xff,0xff,43,0xff,0xff,55,0xff,43,0xff,0xff,50,0xff,0xff,38,0xff };

static const uint8_t LEAD_A[16] = { 69,0xff,72,0xff,76,0xff,74,0xff,72,0xff,71,0xff,72,0xff,76,0xff };
static const uint8_t LEAD_B[16] = { 65,0xff,69,0xff,72,0xff,69,0xff,65,0xff,67,0xff,69,0xff,72,0xff };
static const uint8_t LEAD_C[16] = { 67,0xff,72,0xff,76,0xff,79,0xff,76,0xff,74,0xff,72,0xff,67,0xff };
static const uint8_t LEAD_D[16] = { 62,0xff,67,0xff,71,0xff,74,0xff,71,0xff,69,0xff,67,0xff,62,0xff };

static const uint8_t PAD_A[16] = { 57,0xff,0xff,0xff,60,0xff,0xff,0xff,64,0xff,0xff,0xff,69,0xff,0xff,0xff };
static const uint8_t PAD_B[16] = { 53,0xff,0xff,0xff,57,0xff,0xff,0xff,60,0xff,0xff,0xff,65,0xff,0xff,0xff };
static const uint8_t PAD_C[16] = { 60,0xff,0xff,0xff,64,0xff,0xff,0xff,67,0xff,0xff,0xff,72,0xff,0xff,0xff };
static const uint8_t PAD_D[16] = { 55,0xff,0xff,0xff,59,0xff,0xff,0xff,62,0xff,0xff,0xff,67,0xff,0xff,0xff };

static const uint8_t PLK_A[16] = { 0xff,0xff,64,0xff,0xff,0xff,64,0xff,0xff,0xff,64,0xff,0xff,0xff,64,0xff };
static const uint8_t PLK_B[16] = { 0xff,0xff,60,0xff,0xff,0xff,60,0xff,0xff,0xff,60,0xff,0xff,0xff,60,0xff };
static const uint8_t PLK_C[16] = { 0xff,0xff,67,0xff,0xff,0xff,67,0xff,0xff,0xff,67,0xff,0xff,0xff,67,0xff };
static const uint8_t PLK_D[16] = { 0xff,0xff,62,0xff,0xff,0xff,62,0xff,0xff,0xff,62,0xff,0xff,0xff,62,0xff };

static const uint8_t DRUM_IDS[16]  = { 0,0xff,3,0xff,1,0xff,3,0xff,0,0xff,3,0xff,1,0xff,3,0xff };
static const uint8_t DRUM_VELS[16] = { 14,0,7,0,12,0,7,0,14,0,7,0,12,0,7,0 };
static const uint8_t DRUM_IDS_B[16]  = { 0,0xff,3,0xff,1,0xff,3,0xff,0,0xff,3,0xff,1,0xff,3,4 };
static const uint8_t DRUM_IDS_C[16]  = { 0,0xff,3,0xff,1,0xff,3,5,0,0xff,3,0xff,1,0xff,3,0xff };
static const uint8_t DRUM_IDS_D[16]  = { 0,0xff,3,0xff,1,0xff,3,0xff,0,0xff,3,4,1,0xff,3,4 };

static void demo_one(int p, const uint8_t *bass, const uint8_t *lead,
                     const uint8_t *pad, const uint8_t *plk,
                     const uint8_t *arp, const uint8_t *hi) {
  seq16(p, ST_TRK_FM1, bass, 14, 7);
  seq16(p, ST_TRK_FM2, lead, 14, 9);
  seq16(p, ST_TRK_FM3, pad, 12, 15);
  arp16(p, ST_TRK_FM4, arp, 4, 12, 4);
  seq16(p, ST_TRK_FM5, plk, 12, 3);
  arp16(p, ST_TRK_SSG1, hi, 4, 12, 2);
  seq16(p, ST_TRK_SSG2, pad, 11, 11);
  for (int s = 0; s < ST_STEPS; s++)
    if ((s & 3) == 2) put(p, ST_TRK_NOISE, s, 0, 10, 2);
}

/* Muted tracks are bypassed inside the OPNA core (not merely un-triggered):
   this is what keeps the real-time budget manageable when FM4-6 are off. */
static void update_mask(void) {
  unsigned m = 0;
  for (int c = 0; c < 6; c++)
    if (S->mute[c] && !(c == 5 && E.live_focus)) m |= 1u << c;
  /* A silent SSG channel is masked too: when all three are silent the whole
     SSG mixer is skipped, and while one is active the others cost nothing. */
  if (S->mute[ST_TRK_SSG1] || E.ssg_vol[0] == 0) m |= 1u << 6;
  if (S->mute[ST_TRK_SSG2] || E.ssg_vol[1] == 0) m |= 1u << 7;
  if (S->mute[ST_TRK_NOISE] || E.ssg_vol[2] == 0) m |= 1u << 8;
  if (S->mute[ST_TRK_DRUM]) m |= 0x7e00u;
  opna_set_mask(&E.opna, m);
}

void studio_clear_song(void) {
  clear_steps();
  memset(S->mute, 0, sizeof(S->mute));
  /* PC-9801-26K-style default: 3 FM (bass/lead/pad), as the reference project
     does.  FM4-6 are one C press away in MIX, but they cost real CPU. */
  S->mute[ST_TRK_FM4] = 1;
  S->mute[ST_TRK_FM5] = 1;
  S->mute[ST_TRK_FM6] = 1;
  S->mute[ST_TRK_SSG2] = 1;   /* CPU default: SSG1 arp + drums only */
  S->mute[ST_TRK_NOISE] = 1;  /* drums already provide hats */
  static const uint8_t lv[ST_TRACKS] = { 14, 13, 11, 12, 11, 10, 10, 9, 8, 13 };
  for (int t = 0; t < ST_TRACKS; t++) S->level[t] = lv[t];
  for (int c = 0; c < 6; c++) S->patch[c] = (uint8_t)c;
  update_mask();
}

void studio_demo_song(void) {
  studio_clear_song();
  demo_one(0, BASS_A, LEAD_A, PAD_A, PLK_A, ARP_A, HI_A);
  demo_one(1, BASS_B, LEAD_B, PAD_B, PLK_B, ARP_B, HI_B);
  demo_one(2, BASS_C, LEAD_C, PAD_C, PLK_C, ARP_C, HI_C);
  demo_one(3, BASS_D, LEAD_D, PAD_D, PLK_D, ARP_D, HI_D);
  /* FM6: sparse sparkle (the live layer takes this channel on the SYN page) */
  put(0, ST_TRK_FM6, 3, 81, 8, 5);
  put(0, ST_TRK_FM6, 11, 76, 8, 5);
  put(1, ST_TRK_FM6, 4, 84, 8, 5);
  put(1, ST_TRK_FM6, 12, 77, 8, 5);
  put(2, ST_TRK_FM6, 6, 91, 8, 5);
  put(2, ST_TRK_FM6, 14, 88, 8, 5);
  put(3, ST_TRK_FM6, 2, 86, 8, 5);
  put(3, ST_TRK_FM6, 10, 83, 8, 5);
  drums16(0, DRUM_IDS, DRUM_VELS);
  drums16(1, DRUM_IDS_B, DRUM_VELS);
  drums16(2, DRUM_IDS_C, DRUM_VELS);
  drums16(3, DRUM_IDS_D, DRUM_VELS);
}

/* ------------------------------------------------------------------------- */
/* Timing                                                                    */
/* ------------------------------------------------------------------------- */
static uint32_t step_frames_for(int bpm) {
  if (bpm < 60) bpm = 60;
  if (bpm > 200) bpm = 200;
  return (PFM_MIX_RATE * 60u) / ((uint32_t)bpm * 4u);
}
static uint32_t swing_frames_for(uint32_t step, int pct) {
  if (pct < 0) pct = 0;
  if (pct > 50) pct = 50;
  return (uint32_t)((uint64_t)step * (uint32_t)pct / 200u);
}

static void schedule_next(int from_step) {
  int s = from_step + 1;
  if (s >= ST_STEPS) {
    s = 0;
    E.bar_start += ST_STEPS * E.step_frames;
    E.next_step = E.bar_start;
    if (E.chain) E.pattern = (E.pattern + 1) & (ST_PATTERNS - 1);
  } else {
    uint32_t t = E.bar_start + (uint32_t)s * E.step_frames;
    if (s & 1) t += E.swing_frames;
    E.next_step = t;
  }
}

static uint32_t gate_frames(const struct st_step *s) {
  uint32_t g = E.step_frames * (uint32_t)(s->gate + 1) / 16u;
  if (g < 64) g = 64;
  if (g > E.step_frames) g = E.step_frames;
  return g;
}

static void seq_fire(int step) {
  E.step = step;
  E.step_next = (step + 1) & (ST_STEPS - 1);
  for (int t = 0; t < ST_TRACKS; t++) {
    struct st_step *s = &S->step[E.pattern][t][step];
    if (t == ST_TRK_FM6 && E.live_focus) {
      if (E.note_active[t]) { fm_note_off(t); E.note_active[t] = 0; }
      continue;
    }
    if (s->note == 0xff || S->mute[t] || S->level[t] == 0) {
      if (t < 6 && E.note_active[t]) { fm_note_off(t); E.note_active[t] = 0; }
      if ((t == ST_TRK_SSG1 || t == ST_TRK_SSG2) && E.note_active[t]) {
        ssg_off(t - ST_TRK_SSG1);
        E.note_active[t] = 0;
      }
      if (t == ST_TRK_NOISE && E.note_active[t]) { E.ssg_decay[2] = 4; E.note_active[t] = 0; }
      continue;
    }
    switch (t) {
    case 0: case 1: case 2: case 3: case 4: case 5:
      fm_note_on(t, s->note, s->vel);
      E.note_active[t] = 1;
      E.note_off_at[t] = (s->gate >= 15) ? 0 : E.pos + gate_frames(s);
      break;
    case ST_TRK_SSG1: case ST_TRK_SSG2:
      ssg_note_on(t - ST_TRK_SSG1, s->note, s->vel, S->level[t]);
      E.note_active[t] = 1;
      E.note_off_at[t] = (s->gate >= 15) ? 0 : E.pos + gate_frames(s);
      break;
    case ST_TRK_NOISE:
      noise_hit(s->vel, S->level[t]);
      E.note_active[t] = 1;
      E.note_off_at[t] = E.pos + gate_frames(s);
      break;
    default:
      drum_hit(s->note, s->vel);
      break;
    }
  }
  schedule_next(step);
  update_mask();
}

/* ------------------------------------------------------------------------- */
/* Live layer                                                                */
/* ------------------------------------------------------------------------- */
static const uint8_t ARP_NOTES[8] = { 0, 3, 7, 10, 12, 10, 7, 3 };

static void live_apply_bend(void) {
  if (E.live_sounding) fm_freq(5, E.live_base + E.bend);
}

static void live_stop(void) {
  if (E.live_focus) fm_note_off(5);
  E.live_sounding = 0;
  E.arp_idx = 0;
}

static void arp_fire(void) {
  if (E.live_note <= 0) return;
  int base = E.live_note + ARP_NOTES[E.arp_idx & 7];
  E.live_base = base;
  E.live_sounding = base + E.bend;
  fm_note_on(5, E.live_sounding, 12);
  E.arp_idx++;
  E.arp_at += E.step_frames;
  if ((int32_t)(E.arp_at - E.pos) < 0)
    E.arp_at = E.pos + E.step_frames;
}

void studio_live_focus(int on) {
  E.live_focus = on ? 1 : 0;
  if (!on) {
    live_stop();
    E.live_held = E.live_latched = 0;
  }
  update_mask();
}
int studio_live_focus_get(void) { return E.live_focus; }

void studio_live_key(int note, int vel) {
  if (note < 0) {
    E.live_held = 0;
    if (!E.live_latched) {
      if (E.live_arp) live_stop();
      else if (E.live_focus) { fm_note_off(5); E.live_sounding = 0; }
    }
    return;
  }
  if (note > 127) note = 127;
  E.live_note = note;
  E.live_held = 1;
  E.arp_idx = 0;
  if (E.live_arp) {
    E.arp_at = E.pos;
  } else if (E.live_focus) {
    E.live_base = note;
    E.live_sounding = note + E.bend;
    fm_note_on(5, E.live_sounding, vel);
  }
}

void studio_live_off(void) {
  E.live_held = E.live_latched = 0;
  live_stop();
}

void studio_live_patch(int patch) {
  E.live_patch = patch % ST_PATCHES;
  if (E.live_focus) {
    fm_write_patch(5, E.live_patch);
    if (E.live_sounding) fm_note_on(5, E.live_sounding, 12);
  }
}
int studio_live_patch_get(void) { return E.live_patch; }

void studio_live_arp(int on) {
  E.live_arp = on ? 1 : 0;
  if (on) {
    E.arp_idx = 0;
    E.arp_at = E.pos;
  } else if (!E.live_held) {
    live_stop();
  } else if (E.live_focus) {
    E.live_base = E.live_note;
    E.live_sounding = E.live_note + E.bend;
    fm_note_on(5, E.live_sounding, 12);
  }
}
int studio_live_arp_get(void) { return E.live_arp; }

void studio_live_latch(int on) {
  E.live_latched = on ? 1 : 0;
  if (!on && !E.live_held) live_stop();
}
int studio_live_latch_get(void) { return E.live_latched; }

int studio_live_note(void) { return E.live_sounding ? E.live_sounding : -1; }
int studio_live_active(void) { return (E.live_sounding != 0); }

void studio_live_tilt(int x, int y) {
  if (x < -1000) x = -1000;
  if (x > 1000) x = 1000;
  if (y < -1000) y = -1000;
  if (y > 1000) y = 1000;
  E.tilt_x = x;
  E.tilt_y = y;
  int bend = x / 500;
  if (bend != E.bend) {
    E.bend = bend;
    live_apply_bend();
  }
}
int studio_live_tilt_x(void) { return E.tilt_x / 10; }
int studio_live_tilt_y(void) { return E.tilt_y / 10; }

/* ------------------------------------------------------------------------- */
/* Lifecycle / transport                                                     */
/* ------------------------------------------------------------------------- */
void studio_init(void) {
  memset(&E, 0, sizeof(E));
  opna_reset(&E.opna);
  E.bpm = 124;
  E.swing = 12;
  E.chain = 0;   /* boot default: static SEQ grid; SYS can enable */
  E.pattern = 0;
  E.live_patch = 1;
  E.analysis = 0;
  E.step_frames = step_frames_for(E.bpm);
  E.swing_frames = swing_frames_for(E.step_frames, E.swing);
  studio_clear_song();
  for (int c = 0; c < 6; c++) {
    g_active_patch[c] = S->patch[c];
    fm_write_patch(c, S->patch[c]);
    for (int s = 0; s < 4; s++)
      g_base_tl[c][s] = st_patches[S->patch[c] % ST_PATCHES].op[s].tl;
  }
  opna_writereg(&E.opna, 0x07, 0x1C);   /* tone A/B on, tone C off, noise C on */
  opna_writereg(&E.opna, 0x06, 0x18);
  opna_writereg(&E.opna, 0x08, 0);
  opna_writereg(&E.opna, 0x09, 0);
  opna_writereg(&E.opna, 0x0A, 0);
  opna_writereg(&E.opna, 0x11, 0x2A);   /* rhythm total level (balanced) */
  for (int d = 0; d < 6; d++) opna_writereg(&E.opna, 0x18u + (unsigned)d, 0xDF);
  st_fft_init();
}

struct st_song *studio_song(void) { return S; }

void studio_sync_song(void) {
  for (int c = 0; c < 6; c++) {
    int pi = S->patch[c] % ST_PATCHES;
    if (g_active_patch[c] != pi) {
      fm_note_off(c);
      E.note_active[c] = 0;
      g_active_patch[c] = pi;
      fm_write_patch(c, pi);
      for (int s = 0; s < 4; s++) g_base_tl[c][s] = st_patches[pi].op[s].tl;
    }
    if (E.note_active[c]) fm_tl_update(c);
    if (S->mute[c] && E.note_active[c]) { fm_note_off(c); E.note_active[c] = 0; }
  }
  for (int t = ST_TRK_SSG1; t <= ST_TRK_SSG2; t++) {
    if (S->mute[t] && E.note_active[t]) { ssg_off(t - ST_TRK_SSG1); E.note_active[t] = 0; }
    else if (E.note_active[t]) {
      int v = S->level[t];
      opna_writereg(&E.opna, 0x08u + (unsigned)(t - ST_TRK_SSG1), (unsigned)(v > 15 ? 15 : v));
    }
  }
  if (S->mute[ST_TRK_NOISE]) { E.ssg_decay[2] = 4; E.note_active[ST_TRK_NOISE] = 0; }
  update_mask();
}

void studio_play(int on) {
  if (on && !E.playing) {
    E.bar_start = E.pos;
    E.next_step = E.pos;
    E.step = 0;
    E.step_next = 0;
    E.playing = 1;
  } else if (!on) {
    E.playing = 0;
    E.step = 0;
    E.step_next = 0;
    for (int t = 0; t < ST_TRACKS; t++) {
      if (t < 6 && E.note_active[t]) fm_note_off(t);
      if ((t == ST_TRK_SSG1 || t == ST_TRK_SSG2) && E.note_active[t]) ssg_off(t - ST_TRK_SSG1);
      if (t == ST_TRK_NOISE) E.ssg_decay[2] = 4;
      E.note_active[t] = 0;
    }
    update_mask();
  }
}
int studio_playing(void) { return E.playing; }
void studio_select_pattern(int p) { E.pattern = ((p % ST_PATTERNS) + ST_PATTERNS) % ST_PATTERNS; }
int studio_pattern(void) { return E.pattern; }
int studio_step(void) { return E.step; }
int studio_bpm(void) { return E.bpm; }
int studio_swing(void) { return E.swing; }
int studio_chain(void) { return E.chain; }
uint32_t studio_frames(void) { return E.pos; }

void studio_set_bpm(int bpm) {
  if (bpm < 60) bpm = 60;
  if (bpm > 200) bpm = 200;
  if (bpm == E.bpm) return;
  uint32_t old = E.step_frames;
  uint32_t anchor = E.pos;
  if (E.playing) anchor = E.pos - (uint32_t)E.step * old;
  E.bpm = bpm;
  E.step_frames = step_frames_for(bpm);
  E.swing_frames = swing_frames_for(E.step_frames, E.swing);
  if (E.playing) {
    E.bar_start = anchor;
    int s = E.step + 1;
    if (s >= ST_STEPS) {
      E.bar_start += ST_STEPS * E.step_frames;
      E.next_step = E.bar_start;
      if (E.chain) E.pattern = (E.pattern + 1) & (ST_PATTERNS - 1);
    } else {
      E.next_step = E.bar_start + (uint32_t)s * E.step_frames + (s & 1 ? E.swing_frames : 0);
    }
  }
}

void studio_set_swing(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 50) pct = 50;
  E.swing = pct;
  E.swing_frames = swing_frames_for(E.step_frames, pct);
  if (E.playing) {
    int s = E.step + 1;
    if (s < ST_STEPS)
      E.next_step = E.bar_start + (uint32_t)s * E.step_frames + (s & 1 ? E.swing_frames : 0);
  }
}

void studio_set_chain(int on) { E.chain = on ? 1 : 0; }

/* ------------------------------------------------------------------------- */
/* Preview                                                                   */
/* ------------------------------------------------------------------------- */
void studio_preview(int track, int note) {
  if (track < 0 || track >= ST_TRACKS) return;
  if (track < 6) {
    if (track == 5 && E.live_focus) return;
    fm_note_on(track, note, 12);
    E.note_active[track] = 1;
    E.note_off_at[track] = E.pos + PFM_MIX_RATE / 6;
  } else if (track == ST_TRK_SSG1 || track == ST_TRK_SSG2) {
    ssg_note_on(track - ST_TRK_SSG1, note, 12, 12);
    E.note_active[track] = 1;
    E.note_off_at[track] = E.pos + PFM_MIX_RATE / 5;
  } else if (track == ST_TRK_NOISE) {
    noise_hit(12, 12);
    E.note_active[track] = 0;
  } else {
    drum_hit(note, 12);
  }
  E.activity_until = E.pos + PFM_MIX_RATE / 3;
  update_mask();
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */
static PFM_HOT __attribute__((noinline)) void master_gain(int16_t *buf, unsigned n) {
  for (unsigned i = 0; i < n * 2; i++) {
    int32_t v = (int32_t)buf[i] * 3;
    buf[i] = (int16_t)pfm_clamp16(v);
  }
}

static PFM_HOT __attribute__((noinline)) void capture(const int16_t *buf, unsigned n) {
  int mode = E.analysis;
  if (!mode) return;
  int32_t pl = 0, pr = 0;
  if (mode == 2) {
    for (unsigned i = 0; i < n; i++) {
      int32_t l = buf[i * 2], r = buf[i * 2 + 1];
      E.mono[E.mono_w++ & (ST_FFT_N - 1)] = (int16_t)((l + r) >> 1);
      int32_t al = l < 0 ? -l : l;
      int32_t ar = r < 0 ? -r : r;
      if (al > pl) pl = al;
      if (ar > pr) pr = ar;
    }
  } else {
    /* VU only: a 4-sample peak is plenty at 20 fps and avoids the mono ring. */
    for (unsigned i = 0; i < n; i += 4) {
      int32_t l = buf[i * 2], r = buf[i * 2 + 1];
      int32_t al = l < 0 ? -l : l;
      int32_t ar = r < 0 ? -r : r;
      if (al > pl) pl = al;
      if (ar > pr) pr = ar;
    }
  }
  if (pl > E.vu_hold[0]) E.vu_hold[0] = pl;
  if (pr > E.vu_hold[1]) E.vu_hold[1] = pr;
}

static void ssg_decay_tick(void) {
  for (int ch = 0; ch < 3; ch++) {
    if (E.ssg_decay[ch] && E.ssg_vol[ch]) {
      int v = E.ssg_vol[ch] - E.ssg_decay[ch];
      if (v < 0) v = 0;
      E.ssg_vol[ch] = (uint8_t)v;
      opna_writereg(&E.opna, 0x08u + (unsigned)ch, (unsigned)v);
    }
  }
  update_mask();
}

PFM_HOT void studio_render(int16_t *buf, int frames) {
  while (frames > 0) {
    for (int t = 0; t < ST_TRACKS; t++) {
      if (E.note_active[t] && E.note_off_at[t] && (int32_t)(E.pos - E.note_off_at[t]) >= 0) {
        if (t < 6) fm_note_off(t);
        else if (t == ST_TRK_SSG1 || t == ST_TRK_SSG2) ssg_off(t - ST_TRK_SSG1);
        else if (t == ST_TRK_NOISE) E.ssg_decay[2] = 4;
        E.note_active[t] = 0;
      }
    }
    update_mask();
    if (E.playing && (int32_t)(E.pos - E.next_step) >= 0) seq_fire(E.step_next);
    if (E.live_arp && E.live_focus && E.live_note > 0 &&
        (E.live_held || E.live_latched) && (int32_t)(E.pos - E.arp_at) >= 0) {
      arp_fire();
    }
    if ((int32_t)(E.pos - E.ssg_next) >= 0) {
      ssg_decay_tick();
      E.ssg_next += 128;
      if ((int32_t)(E.pos - E.ssg_next) >= 0) E.ssg_next = E.pos + 128;
    }

    unsigned n = frames > 256 ? 256u : (unsigned)frames;
    if (E.playing) {
      int32_t d = (int32_t)(E.next_step - E.pos);
      if (d > 0 && (unsigned)d < n) n = (unsigned)d;
    }
    for (int t = 0; t < ST_TRACKS; t++) {
      if (E.note_active[t]) {
        int32_t d = (int32_t)(E.note_off_at[t] - E.pos);
        if (d > 0 && (unsigned)d < n) n = (unsigned)d;
      }
    }
    if (E.live_arp && E.live_focus && E.live_note > 0 &&
        (E.live_held || E.live_latched)) {
      int32_t d = (int32_t)(E.arp_at - E.pos);
      if (d > 0 && (unsigned)d < n) n = (unsigned)d;
    }
    if (n == 0) continue;

    opna_mix(&E.opna, buf, n);
    master_gain(buf, n);
    capture(buf, n);
    buf += (size_t)n * 2;
    frames -= (int)n;
    E.pos += n;
  }
}

/* ------------------------------------------------------------------------- */
/* Analysis                                                                  */
/* ------------------------------------------------------------------------- */
static int16_t g_scope[128];

static void level_update(void) {
  for (int ch = 0; ch < 2; ch++) {
    int32_t v = E.vu_hold[ch] * 100 / 24000;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    E.level_pct[ch] = (int)v;
    E.vu_hold[ch] -= E.vu_hold[ch] >> 3;
  }
}

void studio_set_analysis(int mode) { E.analysis = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }
void studio_vu_update(void) { if (E.analysis == 1) level_update(); }

void studio_analysis_update(void) {
  if (E.analysis != 2) return;
  int16_t tmp[ST_FFT_N];
  unsigned base = E.mono_w - ST_FFT_N;
  for (unsigned i = 0; i < ST_FFT_N; i++)
    tmp[i] = E.mono[(base + i) & (ST_FFT_N - 1)];
  st_fft_analyze(tmp);
  /* Trigger the scope on a rising zero crossing so the trace stands still. */
  unsigned start = E.mono_w - ST_FFT_N;
  for (unsigned i = 2; i < ST_FFT_N - 128; i++) {
    int a = E.mono[(start + i - 1) & (ST_FFT_N - 1)];
    int b = E.mono[(start + i) & (ST_FFT_N - 1)];
    if (a < 0 && b >= 0) { start += i; break; }
  }
  for (int i = 0; i < 128; i++)
    g_scope[i] = E.mono[(start + (unsigned)i * 2) & (ST_FFT_N - 1)];
  level_update();
}

int studio_is_idle(void) {
  if (E.playing) return 0;
  if (E.live_sounding) return 0;
  if (E.live_arp && (E.live_held || E.live_latched)) return 0;
  if ((int32_t)(E.activity_until - E.pos) > 0) return 0;
  for (int t = 0; t < ST_TRACKS; t++) if (E.note_active[t]) return 0;
  return 1;
}

const uint8_t *studio_spectrum(void) { return st_fft_spec(); }
const uint8_t *studio_spectrum_peak(void) { return st_fft_peak(); }
const int16_t *studio_scope(void) { return g_scope; }
int studio_level(int ch) { return E.level_pct[ch & 1]; }
uint32_t studio_signal_frames(void) { return E.pos; }
