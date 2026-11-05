# NOTICE — third-party code and changes

**OPNA Studio P2 v1.0.0 · 2026-11**

OPNA Studio P2 is an independent firmware project. It reuses, with attribution,
the OPNA sound core and board-support foundation of **pocket-fmdsp** (Apache-2.0).
The reference repository itself was treated as read-only; the copy under
`src/opna`, `include/pfm`, `firmware/bsp`, and `firmware/gui` is the basis for
this project.

## OPNA core / STM32 Primer2 BSP / font renderer

Files:

- `src/opna/opna.c`, `opna_fm.c`, `opna_ssg.c`, `opna_drum.c`, `opna_drum_pcm.h`,
  `opna_tables.h`
- `include/pfm/opna*.h`, `pfm_config.h`, `pfm_prof.h`
- `firmware/bsp/primer2/*` (LCD / FSMC, joystick, STW5094A codec, SD-SPI, power)
- `firmware/bsp/sim/*` (semihosting backend, extended for the studio)
- `firmware/gui/gfx.c`, `font_sjis.h` (font: Misaki gothic 8×8)
- `firmware/ld/*`, `firmware/startup/startup.c`

Origin: **pocket-fmdsp** — <https://github.com/myon98/98fmplayer> is the
correctness reference for the OPNA/PMD semantics; pocket-fmdsp is the source of
this optimized, integer-only Cortex-M3 implementation. pocket-fmdsp is licensed
under the **Apache License 2.0** (see [`LICENSE`](LICENSE)).

The OPNA emulator is in turn based on **libopna / 98fmplayer** by Takamichi
Horikawa, **BSD 2-Clause**:

```
Copyright (c) 2016 Takamichi Horikawa
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Modification of the copied OPNA core

`src/opna/opna_ssg.c`: the captured YMF288 `voltable[32]` linear volume table is
scaled **×4** so the SSG block balances the FM and rhythm blocks in this
instrument. This is the only intentional change to the audio-level behaviour of
the copied core. The FM and rhythm paths are unchanged and remain bit-exact with
the reference. The change is marked with a comment at the table.

`firmware/bsp/primer2/board_primer2.c` adds the LIS3LV02 accelerometer driver,
the 4-wire touch ADC path, the three-point calibration, FatFs save/load entry
points, and the one-window `board_lcd_blit()` fast path.
`firmware/bsp/sim/board_sim.c` was rewritten for the studio (screenshots, WAV,
host files, synthetic tilt).

## Touch input references

The 4-wire ADC mapping follows the arithmetic used by **Chopstx**
(`example-primer2/primer2-ts.c`); Chopstx is GPLv3+ with an additional
permission, and no Chopstx source is included here.  The mapping was
reimplemented in this project for the ADC3/PC0–PC3 wiring.

The three-point calibration is based on the affine solve and the Primer2
reference points in **CircleOS** (Keolabs)
`TOUCHSCR_CalculateCalibration()` / `touchscreen_spe.c`.  The CircleOS routine
credits the public-domain calibration sample by **Carlos E. Vidales**.  This
project stores the six raw points in STM32 backup registers and rebuilds the
matrix at boot; it does not include the CircleOS scheduler or UI.

## Misaki gothic 8×8 font

`firmware/gui/font_sjis.h` is generated from the **Misaki gothic** 8×8 BDF by
littlelimit.net, released as **public domain**. The glyph table is a derived
bitmap of that font.

## FreeRTOS

`firmware/freertos/` is the FreeRTOS kernel, **MIT License**, Copyright (C)
Amazon.com, Inc. or its affiliates. See the source headers.

## FatFs

`firmware/fatfs/` is ChaN's FatFs, **BSD 1-Clause License**. See `ff.c` / `ff.h`.

## Original work in this project

Unless stated above, the following are original to OPNA Studio P2:

- `firmware/studio/engine.c` — sequencer, live layer, register scheduling
- `firmware/studio/ui.c`, `gfx_ext.c` — five-page UI, input state machine,
  drawing primitives, splash
- `firmware/studio/fft.c` — 256-point FFT, spectrum mapping, scope trigger
- `firmware/studio/patches.c` — the 8 FM voices
- `firmware/studio/app.c` — boot, SD persistence, RTOS tasks, QEMU showcase
- `host/studio_render.c`, `firmware/tools/*`, `scripts/*`, all documentation
  and media.

The 24-second demo song, the UI layout, and all screen captures were created for
this project.
