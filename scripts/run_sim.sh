#!/bin/sh
# Build (if needed) and run OPNA Studio P2 in QEMU, then collect the generated
# screenshots + WAV into build-fw/sim-run/ and make a contact sheet.
#
#   QEMU=/path/to/qemu-system-arm scripts/run_sim.sh
#   PYTHON=python scripts/run_sim.sh
set -e
cd "$(dirname "$0")/.."

ELF=build-fw/opna_studio_sim.elf
OUT=build-fw/sim-run
QEMU=${QEMU:-qemu-system-arm}
PY=${PYTHON:-python3}

[ -f "$ELF" ] || { echo "build first: cmake --build build-fw"; exit 1; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "qemu-system-arm not found (set QEMU=...)"; exit 1; }
command -v "$PY" >/dev/null 2>&1 || PY=python
"$PY" -c "" >/dev/null 2>&1 || PY=python

mkdir -p "$OUT"
rm -f sim_shot_*.ppm sim_play_*.wav
echo "== running $ELF =="
"$QEMU" -M netduino2 -cpu cortex-m3 -nographic \
  -semihosting-config enable=on,target=native -kernel "$ELF"

rm -f "$OUT"/*.ppm "$OUT"/*.wav "$OUT"/*.png
mv -f sim_shot_*.ppm sim_play_*.wav "$OUT"/ 2>/dev/null || true

"$PY" - "$OUT" <<'PY'
import glob, os, sys
try:
    from PIL import Image
except Exception:
    print("PIL not available; skipping contact sheet")
    sys.exit(0)
d = sys.argv[1]
shots = sorted(glob.glob(os.path.join(d, "sim_shot_*.ppm")))
if not shots:
    sys.exit(0)
# 6 per row, 2x nearest-neighbour
sel = shots[::max(1, len(shots) // 12)][:12]
imgs = [Image.open(s).convert("RGB").resize((256, 320), Image.NEAREST) for s in sel]
cols = 6
rows = (len(imgs) + cols - 1) // cols
pad = 8
sheet = Image.new("RGB", (cols * (256 + pad) + pad, rows * (320 + pad) + pad), (24, 26, 36))
for i, im in enumerate(imgs):
    c, r = i % cols, i // cols
    sheet.paste(im, (pad + c * (256 + pad), pad + r * (320 + pad)))
sheet.save(os.path.join(d, "contact_sheet.png"))
print("wrote", os.path.join(d, "contact_sheet.png"), "from", len(imgs), "frames")
PY

echo "== artifacts in $OUT =="
ls -1 "$OUT"
