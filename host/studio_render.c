/*
 * Host renderer for the OPNA Studio engine (no LCD / no board).
 * Renders the built-in demo song to a WAV so the engine can be validated and
 * auditioned without flashing hardware or booting QEMU.
 */
#include "studio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void put32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "studio_demo.wav";
  int seconds = argc > 2 ? atoi(argv[2]) : 24;
  if (seconds < 1) seconds = 1;

  FILE *f = fopen(out, "wb");
  if (!f) { perror(out); return 1; }
  uint8_t hdr[44];
  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr, "RIFF", 4);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  put32le(hdr + 16, 16);
  put16le(hdr + 20, 1);
  put16le(hdr + 22, 2);
  put32le(hdr + 24, PFM_MIX_RATE);
  put32le(hdr + 28, PFM_MIX_RATE * 4);
  put16le(hdr + 32, 4);
  put16le(hdr + 34, 16);
  memcpy(hdr + 36, "data", 4);
  fwrite(hdr, 1, sizeof(hdr), f);

  studio_init();
  studio_demo_song();
  if (argc > 3) {
    int solo = atoi(argv[3]);
    struct st_song *song = studio_song();
    for (int t = 0; t < ST_TRACKS; t++) if (t != solo) song->mute[t] = 1;
  }
  studio_play(1);

  int16_t buf[512 * 2];
  uint32_t total = (uint32_t)seconds * PFM_MIX_RATE;
  uint32_t done = 0, sec = 0;
  int32_t peak = 0, sec_peak = 0;
  uint32_t clipped = 0, nonzero = 0;
  uint32_t sec_clipped = 0, sec_nonzero = 0;
  while (done < total) {
    unsigned n = total - done;
    if (n > 512) n = 512;
    studio_render(buf, (int)n);
    fwrite(buf, sizeof(int16_t) * 2, n, f);
    for (unsigned i = 0; i < n * 2; i++) {
      int32_t v = buf[i];
      int32_t a = v < 0 ? -v : v;
      if (a > peak) peak = a;
      if (a > sec_peak) sec_peak = a;
      if (a >= 32767) { clipped++; sec_clipped++; }
      if (v) { nonzero++; sec_nonzero++; }
    }
    done += n;
    if (done >= (sec + 1) * PFM_MIX_RATE) {
      printf("  t=%2us  peak=%5d  clipped=%5u  active=%3u%%\n",
             sec + 1, sec_peak, sec_clipped,
             (unsigned)((uint64_t)sec_nonzero * 100 / ((uint64_t)PFM_MIX_RATE * 4)));
      sec++;
      sec_peak = 0; sec_clipped = 0; sec_nonzero = 0;
    }
  }
  /* patch sizes */
  uint32_t bytes = done * 4;
  put32le(hdr + 4, 36 + bytes);
  put32le(hdr + 40, bytes);
  fseek(f, 0, SEEK_SET);
  fwrite(hdr, 1, sizeof(hdr), f);
  fclose(f);

  printf("wrote %s: %u frames (%d s), peak %d, clipped %u, active %u%%\n",
         out, done, seconds, peak, clipped,
         (unsigned)((uint64_t)nonzero * 100 / ((uint64_t)done * 2)));
  return 0;
}
