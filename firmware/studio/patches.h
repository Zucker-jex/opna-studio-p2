#ifndef STUDIO_PATCHES_H
#define STUDIO_PATCHES_H

#include <stdint.h>

/* One FM operator in physical-slot order.  Register mapping (slot 0..3):
 *   0x30+MUL/DET  0x40+TL  0x50+KS/AR  0x60+DR  0x70+SR  0x80+SL/RR
 * Operators are stored in the hardware's physical order S1,S3,S2,S4. */
struct st_op {
  uint8_t mul, det, tl, ks, ar, dr, sr, sl, rr;
};

struct st_patch {
  uint8_t alg, fb;
  struct st_op op[4];
};

#define ST_PATCHES 8

extern const struct st_patch st_patches[ST_PATCHES];
extern const char *const st_patch_name[ST_PATCHES];

/* Bitmask of physical slots that are carriers for the given algorithm. */
static inline uint8_t st_patch_carriers(unsigned alg) {
  switch (alg) {
  case 0: case 1: case 2: case 3: return 0x8;        /* S4 */
  case 4: return 0xA;                                 /* S3 + S4 */
  case 5: case 6: return 0xE;                         /* S3 + S2 + S4 */
  default: return 0xF;                                /* all */
  }
}

#endif
