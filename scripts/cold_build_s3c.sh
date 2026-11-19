#!/usr/bin/env bash
# S3c A-grade reproducible cold build. Usage: cold_build_s3c.sh <out_dir>
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$1"
EPOCH=1794813449
BOOT=/root/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

echo "[1/5] double-delete .pio/build/eego_a4 and .cache"
rm -rf .pio/build/eego_a4 .cache

echo "[2/5] source build env WITHOUT pipe + epoch assert"
# No 'source | grep': that runs source in a subshell and loses the export.
if [ -f scripts/build_env.sh ]; then
  source scripts/build_env.sh
fi
export SOURCE_DATE_EPOCH="$EPOCH"
if [ "$SOURCE_DATE_EPOCH" != "$EPOCH" ]; then
  echo "FATAL: SOURCE_DATE_EPOCH not exported (got '$SOURCE_DATE_EPOCH')"; exit 1
fi
echo "  SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH OK"

echo "[3/5] pio run -e eego_a4 (jobs=3 to stay under cgroup 8GB cap)"
# nproc=32 but the sandbox cgroup caps RAM at 8GB; 32 parallel cc1plus get OOM-killed.
pio run -e eego_a4 -j 3

echo "[4/5] archive 4 segments -> $OUT"
mkdir -p "$OUT"
python3.11 scripts/archive_segments.py \
  --build-dir .pio/build/eego_a4 \
  --boot-app0 "$BOOT" \
  --out "$OUT/firmware.factory.bin" \
  --manifest "$OUT/MANIFEST.json" \
  --source-date-epoch "$EPOCH"

echo "[5/5] checksums"
cp .pio/build/eego_a4/firmware.bin "$OUT/firmware.app.bin"
cp .pio/build/eego_a4/firmware.elf "$OUT/firmware.elf"
sha256sum "$OUT/firmware.app.bin" "$OUT/firmware.factory.bin" | tee "$OUT/SHA256SUMS.txt"
echo "COLD BUILD DONE -> $OUT"
