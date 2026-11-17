#!/usr/bin/env bash
# 0x1A scene encoder host assertion — pure host-side batch-B4 judgement.
# gcc + ASan/UBSan, no ESP-IDF / no hardware. Mirrors pico push_sink_host_test.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/push_scene_host_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -I"$ROOT/include" \
   "$ROOT/src/push_scene.c" "$HERE/push_scene_host_test.c" \
   -o "$OUT"
"$OUT"
