# Changelog

## v1.0.0 — 2026-11-06

First public release of OPNA Studio P2 for the STM32 Primer2.

### Highlights

- **15-voice OPNA engine**: 6 FM × 4 operators, 3 SSG, 6 rhythm at 55 466 Hz.
- **10 tracks × 16 steps × 4 patterns** sequencer with swing, per-track level/
  mute, velocity/gate, tie, and A–D pattern chain.
- **8 FM patches**, live FM6 voice, 2-octave keyboard, arpeggiator, LATCH and
  LIS3LV02 tilt bend.
- **Five-page UI** (SEQ / SYN / MIX / VIS / SYS), partial/dirty rendering, VU,
  256-point FFT and triggered scope.
- **Touch**: 4-wire resistive panel on ADC3, top 5 px four-button strip, bottom
  tabs, and **SYS → Touch Cal** three-point calibration (backup-register saved).
- **LIS3LV02 accelerometer** and microSD persistence (`/opna2.p2s`, 1 963 B).
- **QEMU showcase**: 24 s, 474 frames, WAV, self-running five-page demo.
- **Host renderer** sharing `engine.c` for offline audio checks.

### Performance

- Default **3 FM + SSG1 + drums**: 40–50 % CPU, 0 % paused.
- **4 FM** is the clean ceiling (no UND growth); 5 FM starts, 6 FM grows fast;
  all 6 FM + SSG2/NOISE + drums is ≈80 % (stress mode).
- SSG dynamic masking, true idle path, per-page analysis, `-O3` engine,
  `.ramfunc` hot paths, LCD ring gate (`>512`).

### Fixed during bring-up

- 6 FM at 107 % CPU → default 3 FM and masked SSG/NOISE.
- Paused playback still rendering release tails/SSG (57 %) → true idle path.
- Full-page 25 fps redraw causing panel tearing → partial/dirty rendering and
  VIS SRAM canvas.
- Input path redrawing the whole page → BSP clip rectangles / `ui_draw_dirty()`.
- Occasional uncleared partial frame → app `U` / LCD `V_live` snapshot and
  atomic dirty-list take.
- Touch calibration page showing only a full refresh → `publish()` now copies
  `cal`, `cal_step`, `cal_msg` into `V_live`.
- Top four button strip too tall → reduced to the top 5 px; header no longer
  changes page by accident.
- MOD now persists across direction edits; triple C enters/exits.
- Pattern chain defaults to Off; old v1 archives are migrated with FM4–6 /
  SSG2 / NOISE muted.

### Build artifacts

- Flash **225 652 B**, RAM **≈55.0 / 64 KiB**.
- `opna_studio_hw.hex` SHA256:
  `6bb159c7784df8692ad1ece5d239052b05a5fc80957dfb805e0207ddb83260e0`
- `opna_studio_hw.bin` SHA256:
  `09a122c9daf8c83d8751436b233d787446239a5c247172fe7a5b2035e2e39a10`

### Known limitations

- 6 FM is a stress mode; use 4 FM for reliable audio.
- Panel tearing on page changes / VIS canvas flush (TE pin is NC).
- Touch calibration needs VBAT to persist; touch offset is panel-individual.
- Touch supports tap/selection only (no drag/XY pad/sliders).
- Codec I2S runs +0.5 % fast vs the engine (~8 cent), inherited from the BSP.
- No ADPCM-B/PPZ8 streaming, USB-MIDI, recording to SD, or battery gauge yet.
