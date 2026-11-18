#!/usr/bin/env bash
# 0x08 分帧编解码 host 断言 — 方案乙 S1 纯主机侧判据。
# gcc + ASan/UBSan，无 ESP-IDF / 无硬件。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/push_ai_seg_host_test"
SEGASM_OUT="${TMPDIR:-/tmp}/ai_segasm_host_test"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -I"$ROOT/include" \
   "$ROOT/src/push_ai_seg.c" "$HERE/push_ai_seg_host_test.c" \
   -o "$OUT"
"$OUT"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -I"$ROOT/include" \
   "$ROOT/src/push_ai_seg.c" "$ROOT/src/ai_segasm.c" "$HERE/ai_segasm_host_test.c" \
   -o "$SEGASM_OUT"
"$SEGASM_OUT"
