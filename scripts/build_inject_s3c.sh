#!/usr/bin/env bash
# 000-39b1a-t: TEST-ONLY injection build. Produces a firmware with the
# write-failure injection points compiled in. NEVER shipped, NEVER tagged.
# Usage: build_inject_s3c.sh <out_dir>
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$1"
EPOCH=1794813449
BOOT=/root/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

echo "[1/4] double-delete .pio/build/eego_a4-inject and .cache"
rm -rf .pio/build/eego_a4-inject .cache

echo "[2/4] epoch"
export SOURCE_DATE_EPOCH="$EPOCH"
[ "$SOURCE_DATE_EPOCH" = "$EPOCH" ] || { echo "FATAL: epoch"; exit 1; }

echo "[3/4] pio run -e eego_a4-inject -j 2"
mkdir -p "$OUT"
set +e
pio run -e eego_a4-inject -j 2 2>&1 | tee "$OUT/pio_build.log"
RC=${PIPESTATUS[0]}
set -e
[ "$RC" -eq 0 ] || { echo "FATAL: inject build FAILED (rc=$RC)"; exit "$RC"; }

echo "[4/4] archive + checksums"
python3.11 scripts/archive_segments.py \
  --build-dir .pio/build/eego_a4-inject \
  --boot-app0 "$BOOT" \
  --out "$OUT/firmware.factory.bin" \
  --manifest "$OUT/MANIFEST.json" \
  --source-date-epoch "$EPOCH"
cp .pio/build/eego_a4-inject/firmware.bin "$OUT/firmware.app.bin"
sha256sum "$OUT/firmware.app.bin" "$OUT/firmware.factory.bin"
echo "INJECT BUILD DONE -> $OUT"
