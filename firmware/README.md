# firmware — build targets, hardware, QEMU

**OPNA Studio P2 v1.0.0** · STM32F103VET6 · 132 MHz · 55 466 Hz OPNA.

This directory builds two targets from the same sources: the real Primer2
firmware and a QEMU/semihosting simulator. Project overview and controls are in
the [root README](../README.md); algorithms and memory layout are in
[DESIGN.md](../DESIGN.md).

## Targets

| Target | Artifacts | Purpose |
|--------|-----------|---------|
| `opna_studio_hw` | `build-fw/opna_studio_hw.elf/.hex/.bin` | real STM32 Primer2 |
| `opna_studio_sim` | `build-fw/opna_studio_sim.elf` | QEMU `netduino2`; PPM screenshots + WAV |

## Build

```sh
cmake -S firmware -B build-fw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/cmake/arm-none-eabi.cmake
cmake --build build-fw
```

Requires `arm-none-eabi-gcc` on `PATH` (release built with 5.4.1). The toolchain
file is plain `-mcpu=cortex-m3 -mthumb`; no vendor SPL/CMSIS/HAL.

Release image: **225 652 B Flash**, **≈55.0 / 64 KiB RAM**. SHA256 of
`opna_studio_hw.hex`:
`6bb159c7784df8692ad1ece5d239052b05a5fc80957dfb805e0207ddb83260e0`.

## QEMU

```sh
qemu-system-arm -M netduino2 -cpu cortex-m3 -nographic \
  -semihosting-config enable=on,target=native \
  -kernel build-fw/opna_studio_sim.elf
```

Produces `sim_shot_NNN.ppm` and `sim_play_000.wav` in the working directory. The
showcase is self-running (5 pages, 24 s, saves `/opna2.p2s` at 22.2 s).

Optional input script `sim_input.txt` — `<ms> <mask>`, mask bits
`UP=1 DOWN=2 LEFT=4 RIGHT=8 CENTER=16`; the firmware reads it at boot and runs
`ui_input()` instead of the showcase timeline. Without the file the demo drives
itself.

## Performance

- Default mix: **3 FM + SSG1 + drums** (FM1 bass / FM2 lead / FM3 pad, SSG1 high
  arp). FM4–6, SSG2 and NOISE are muted and masked out of the OPNA core; enable
  them in MIX with `C`. The SYN page temporarily unmasks FM6 for the live voice,
  so it runs 4 FM.
- On-board DWT measurements:

| Build | CPU |
|-------|-----|
| old 6-FM | 107 % |
| old 3-FM | 80–90 % (paused 57 %) |
| v1.0 default 3 FM + SSG1 + drums | 40–50 % (paused 0 %) |
| all 6 FM + SSG2/NOISE + drums | ≈80 % |

- **4 FM is the clean ceiling** with this UI: 4 FM has no UND growth, 5 FM
  starts, 6 FM grows fast. 6 FM is a stress mode.
- The big wins: SSG2/NOISE default mute, dynamic SSG masks (skip the whole SSG
  mixer when all three are silent), true idle path (stopped playback writes
  silence without calling `studio_render()`), per-page analysis, `-O3` engine,
  and `studio_render()` + master gain + capture in `.ramfunc`.
- The LCD task only paints when `board_audio_ring_fill() > 512`, so a slow UI
  frame cannot starve the codec.
- QEMU's CPU% is synthetic; on hardware it comes from DWT.

## Display

ST7732 has no double buffer and the TE pin is NC on the Primer2, so tearing can
only be reduced, not eliminated:

- `ui_draw()` full page only on page change;
- `ui_draw_dirty()` for input (BSP clip rects around old/new cell/row/key/
  footer/hint);
- `ui_redraw_pattern()` for the SEQ pattern-chain change;
- `ui_anim()` in steady state, never clearing a large area: VU bars, SYN
  old/new key + info, tilt 3×3 dot, SEQ ruler, status/hint;
- VIS spectrum/scope renders into a 128×73 SRAM canvas and flushes once.

FSMC uses the reference fast timing (`ADDSET=2/ADDHLD=2/DATAST=2/BUSTURN=5`);
backlight PWM stays at 16 kHz. Remaining large writes are page changes and the
VIS canvas flush.

## Input / touch / MEMS

- **Joystick**: PE3–PE6 + PA8, active high.
- **Touch**: 4-wire resistive panel on PC0–PC3, ADC3 injected channels 10–13
  (ADC conversion follows Chopstx `example-primer2/primer2-ts.c`). CircleOS'
  four buttons are the bottom `Y>=132`, 4×32 touch zones; this UI is rotated
  180° (`MADCTL=0xC0`), so they appear at the top. Only the top **5 px** strip
  is the button area, so header taps do not change page. X order is **not**
  mirrored: leftmost=SEQ, then SYN, then MIX, rightmost=VIS. `SYS` stays on the
  bottom tab.
- **Calibration**: `SYS -> Touch Cal`, then `C`. Three crosses at (115,15),
  (115,115), (15,15); the affine solve is based on CircleOS
  `TOUCHSCR_CalculateCalibration()` (Keolabs / public-domain Vidales sample).
  Six raw points are stored in BKP_DR2..7 (magic `0x5A5A`) and re-loaded at
  boot; if the backup domain is invalid, the Chopstx fixed map is used. `C`
  cancels; it exits 1.5 s after the third point. VBAT is required to keep it.
- **MEMS**: LIS3LV02 on SPI1 (PA5 SCK / PA6 MISO / PA7 MOSI / PE2 CS),
  `WHO_AM_I` 0x0F = 0x3A, 200 Hz, three axes. SYN uses tilt for live bend; SYS
  shows `OK x,y` (or N/A when absent).
- **Gestures**: single C = page action (250 ms settle), double C = play/stop,
  triple C = MOD (persistent; another triple exits), long C (0.55 s) = next
  page. C+direction was removed because the five keys are one stick.
- **Cross-task state**: app mutates `U`; `publish()` writes `V_live`; the LCD
  snapshots `V_live` into `V` and takes the dirty list in one critical section.
  This also fixes the occasional uncleared partial frame.
- **Pattern chain** defaults to Off so the SEQ grid is static; `SYS -> Chain`
  enables it (the SYS Demo and QEMU showcase turn it on for A–D).

## Real hardware

- MCU: STM32F103VET6, 12 MHz HSE → **132 MHz** (PLL ×11), APB1 /4, APB2 /2.
- LCD: ST7732 128×160 on FSMC bank1 / NE1, 8-bit 8080 via FSMC_D4..D11
  (each byte shifted <<4), RS = A16.
- Joystick: PE3–PE6 + PA8.
- Accelerometer: LIS3LV02 on SPI1 (PA5/6/7 + PE2 /CS).
- Audio: STW5094A, I2S3 (PB3/PB5/PA15) + DMA2_Ch2, I2C2 control (PB10/PB11).
- SD: bit-banged SD-SPI + FatFs, 8.3 names, `/opna2.p2s` (1 963 B, v2).
- Power: `pwr_support.c` (hold/shutdown).

Flash `opna_studio_hw.hex` with ST-LINK/RLink/STM32CubeProgrammer. Insert a
FAT32 microSD for persistence; without a card the built-in demo plays.

## Known caveats

- The codec path runs at an I2S clock of 55 743 Hz vs the engine's 55 466 Hz
  (+0.5 %, ~8 cent), the same trade-off as the reference BSP.
- 6 FM + SSG2/NOISE + drums is a stress test; use 4 FM for reliable audio.
- Touch calibration is battery-backed (VBAT); if the backup domain is lost,
  run `SYS -> Touch Cal` again. Touch offset is a panel-individual issue.
- Touch supports tap/selection only; no drag, XY pad or sliders.
- Residual tearing is limited to page changes and the VIS canvas flush (TE NC).
- No ADPCM-B/PPZ8 streaming, USB-MIDI, or recording to SD in v1.0.
