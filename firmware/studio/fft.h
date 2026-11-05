#ifndef FFT_H
#define FFT_H

#include <stdint.h>

#define ST_FFT_N 256

void st_fft_init(void);
/* Analyse 256 consecutive mono samples (int16).  Fills the 64 display bins. */
void st_fft_analyze(const int16_t *samples);
const uint8_t *st_fft_spec(void);
const uint8_t *st_fft_peak(void);

#endif
