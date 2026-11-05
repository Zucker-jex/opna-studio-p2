/*
 * 256-point fixed-point FFT for the spectrum analyser.
 *
 * The OPNA core renders at 55.5 kHz; a 256-point window is 4.6 ms (216.7 Hz
 * per bin).  This is intentionally not a precision instrument: it runs in the
 * low-priority LCD task at ~20 Hz, uses int32 butterflies (Q15 twiddles) and a
 * log magnitude -> 64 bars mapping tuned for music, with peak hold.
 */
#include "fft.h"
#include "fft_tables.h"
#include <string.h>

static uint8_t g_spec[64];
static uint8_t g_peak[64];
static int32_t g_re[ST_FFT_N];
static int32_t g_im[ST_FFT_N];

void st_fft_init(void) {
  memset(g_spec, 0, sizeof(g_spec));
  memset(g_peak, 0, sizeof(g_peak));
}

void st_fft_analyze(const int16_t *in) {
  for (int i = 0; i < ST_FFT_N; i++) {
    int32_t v = ((int32_t)in[i] * fft_win[i]) >> 15;
    g_re[fft_rev[i]] = v;
    g_im[fft_rev[i]] = 0;
  }

  for (int len = 2; len <= ST_FFT_N; len <<= 1) {
    int half = len >> 1;
    int step = ST_FFT_N / len;
    for (int i = 0; i < ST_FFT_N; i += len) {
      for (int j = 0; j < half; j++) {
        int k = j * step;
        int32_t wr = fft_cos[k], wi = fft_sin[k];
        int32_t ar = g_re[i + j], ai = g_im[i + j];
        int32_t br = g_re[i + j + half], bi = g_im[i + j + half];
        int64_t tr = ((int64_t)wr * br - (int64_t)wi * bi) >> 15;
        int64_t ti = ((int64_t)wr * bi + (int64_t)wi * br) >> 15;
        g_re[i + j] = ar + (int32_t)tr;
        g_im[i + j] = ai + (int32_t)ti;
        g_re[i + j + half] = ar - (int32_t)tr;
        g_im[i + j + half] = ai - (int32_t)ti;
      }
    }
  }

  for (int d = 0; d < 64; d++) {
    uint32_t m = 0;
    for (int b = fft_lo[d]; b <= fft_hi[d]; b++) {
      int32_t re = g_re[b] < 0 ? -g_re[b] : g_re[b];
      int32_t im = g_im[b] < 0 ? -g_im[b] : g_im[b];
      uint32_t t = (uint32_t)(re + im);
      if (t > m) m = t;
    }
    uint32_t q = m >> 7;
    int h = 0;
    if (q) {
      int lg = 31 - __builtin_clz(q);
      int frac;
      if (lg < 10) frac = (int)((q << (10 - lg)) & 1023);
      else if (lg > 10) frac = (int)((q >> (lg - 10)) & 1023);
      else frac = (int)(q & 1023);
      h = (lg - 4) * 6 + frac * 6 / 1024;
    }
    h += d / 16;                     /* gentle high-frequency tilt */
    if (h < 0) h = 0;
    if (h > 63) h = 63;

    if (h >= g_spec[d]) g_spec[d] = (uint8_t)h;
    else if (g_spec[d] > 2) g_spec[d] -= 2;
    else g_spec[d] = 0;

    if (h >= g_peak[d]) g_peak[d] = (uint8_t)h;
    else if (g_peak[d] > 2) g_peak[d] -= 2;
    else g_peak[d] = 0;
  }
}

const uint8_t *st_fft_spec(void) { return g_spec; }
const uint8_t *st_fft_peak(void) { return g_peak; }
