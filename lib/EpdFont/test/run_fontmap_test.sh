#!/usr/bin/env bash
# S3cPF6 — FontMap host regression: build/save/load/corrupt + advance identity
# against FreeType on real TTF samples.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FONT_ROOT="$ROOT/freeink-sdk/libs/font/FreeInkFont"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/fontmap-test.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
INCLUDES="-I$ROOT/lib/EpdFont -I$FONT_ROOT/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"

for src in "$FONT_ROOT"/src/freetype/*.c; do
  cc -std=c11 $INCLUDES -O2 -c "$src" -o "$BUILD/$(basename "$src" .c).o"
done
g++ -std=c++17 -O2 $INCLUDES \
  "$ROOT/lib/EpdFont/FontMap.cpp" \
  "$ROOT/lib/EpdFont/test/fontmap_host_test.cpp" \
  "$BUILD"/ft_*.o -o "$BUILD/fontmap_test"

"$BUILD/fontmap_test" "$ROOT/freeink-sdk/libs/book/FreeInkBook/test/fixtures/fonts/DejaVuSans.ttf"
"$BUILD/fontmap_test" "$ROOT/assets/fonts/builtin.ttf"
echo "FontMap host test: OK"
