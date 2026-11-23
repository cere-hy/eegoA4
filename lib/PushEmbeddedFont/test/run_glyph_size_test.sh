#!/usr/bin/env bash
# S3c P1 — glyph size/baseline alignment host regression.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STBDIR="$ROOT/third_party/freeink-sdk/libs/font/FreeInkFont/third_party/stb"
OUT="${TMPDIR:-/tmp}/glyph_size_test"
g++ -std=c++17 -O2 \
  -I"$ROOT/lib/PushEmbeddedFont/include" -I"$STBDIR" \
  "$ROOT/lib/PushEmbeddedFont/test/glyph_size_host_test.cpp" -o "$OUT"
exec "$OUT" "$ROOT/assets/fonts/builtin.ttf" "$ROOT/assets/fonts/ui-hanzi.bin"
