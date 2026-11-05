#include "patches.h"

/* Curated OPNA voices.  Algorithm carriers (see st_patch_carriers):
 *   alg 0-3: only op[3] sounds; alg 4: op[1]+op[3]; alg 5-6: op[1]+op[2]+op[3];
 *   alg 7: all four.  Modulators run hot (TL 18..35); carriers sit near 0 so
 *   the channel level and velocity have headroom to attenuate them. */
const struct st_patch st_patches[ST_PATCHES] = {
  /* 0 BASS: two modulators into one carrier, feedback growl. */
  { 2, 3, {
    { 1, 0, 24, 0, 31, 12,  0,  0,  8 },
    { 1, 0, 42, 0, 31, 16,  0,  0, 10 },
    { 2, 0, 34, 0, 31, 14,  0,  0, 10 },
    { 1, 0,  3, 1, 31,  8,  2,  2, 10 },
  } },
  /* 1 LEAD: two 2-op stacks, slightly detuned, feedback. */
  { 4, 6, {
    { 2, 0, 22, 1, 31,  8,  1,  1, 10 },
    { 1, 1,  0, 1, 31,  8,  1,  1, 10 },
    { 3, 0, 28, 1, 31, 10,  1,  1, 10 },
    { 1, 2,  4, 1, 31,  8,  1,  1, 10 },
  } },
  /* 2 PAD: slow swell, one modulator feeding three detuned carriers. */
  { 5, 4, {
    { 1, 0, 35, 1, 20, 20,  6,  4,  6 },
    { 1, 1, 10, 1, 18, 20,  6,  4,  6 },
    { 2, 3, 14, 1, 16, 22,  6,  4,  6 },
    { 4, 2, 16, 1, 14, 24,  6,  4,  6 },
  } },
  /* 3 PLUCK: fast attack, full decay to silence. */
  { 4, 5, {
    { 2, 0, 24, 0, 31, 22, 15, 15, 15 },
    { 1, 0,  2, 0, 31, 22, 15, 15, 15 },
    { 3, 0, 30, 0, 31, 24, 15, 15, 15 },
    { 2, 0,  6, 0, 31, 20, 15, 15, 15 },
  } },
  /* 4 BELL: inharmonic, long ring. */
  { 4, 2, {
    { 3, 0, 26, 0, 31, 18, 15, 15, 15 },
    { 1, 0,  8, 0, 31, 16, 15, 15, 15 },
    { 7, 0, 32, 0, 31, 20, 15, 15, 15 },
    { 3, 0, 12, 0, 31, 18, 15, 15, 15 },
  } },
  /* 5 BRASS: slower attack, sustained. */
  { 2, 3, {
    { 1, 0, 20, 1, 18, 10,  3,  2,  8 },
    { 1, 0, 32, 1, 20, 12,  3,  2,  8 },
    { 2, 0, 26, 1, 18, 10,  3,  2,  8 },
    { 1, 0,  0, 1, 20,  8,  2,  1,  8 },
  } },
  /* 6 ORGAN: additive drawbars, no envelope movement. */
  { 7, 0, {
    { 1, 0,  6, 0, 31, 31,  0,  0,  0 },
    { 2, 0, 10, 0, 31, 31,  0,  0,  0 },
    { 4, 0, 14, 0, 31, 31,  0,  0,  0 },
    { 8, 0, 18, 0, 31, 31,  0,  0,  0 },
  } },
  /* 7 GLASS: metallic transient, three carriers. */
  { 5, 3, {
    { 2, 0, 30, 0, 31, 26, 15, 15, 15 },
    { 1, 0,  4, 0, 31, 24, 15, 15, 15 },
    { 4, 0, 12, 0, 31, 28, 15, 15, 15 },
    { 7, 0, 16, 0, 31, 30, 15, 15, 15 },
  } },
};

const char *const st_patch_name[ST_PATCHES] = {
  "BASS", "LEAD", "PAD", "PLUCK", "BELL", "BRASS", "ORGAN", "GLASS",
};
