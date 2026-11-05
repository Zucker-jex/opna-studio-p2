#!/bin/sh
# Encode the QEMU screenshot series + WAV into the demo MP4.
# Run scripts/run_sim.sh first (or point SRC at your own capture).
set -e
cd "$(dirname "$0")/.."

SRC=${SRC:-build-fw/sim-run}
WAV=${WAV:-docs/opna_studio_demo.wav}
OUT=${OUT:-docs/opna_studio_demo.mp4}
FPS=${FPS:-19.75}

[ -d "$SRC" ] || { echo "no capture directory: $SRC"; exit 1; }
[ -f "$WAV" ] || { echo "no audio: $WAV"; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found"; exit 1; }

ffmpeg -y -framerate "$FPS" -i "$SRC/sim_shot_%03d.ppm" -i "$WAV" \
  -vf "scale=512:640:flags=neighbor" \
  -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
  -c:a aac -b:a 192k -movflags +faststart -shortest "$OUT"

echo "wrote $OUT"
