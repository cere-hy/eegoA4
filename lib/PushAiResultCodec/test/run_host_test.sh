#!/usr/bin/env bash
# 0x07 AI 释义接收核 host 断言 — batch B5 纯主机侧判据（含 pthread 并发不撕裂）。
# gcc + ASan/UBSan，无 ESP-IDF / 无硬件。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/push_ai_result_host_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread \
   -I"$ROOT/include" \
   "$ROOT/src/push_ai_result.c" "$HERE/push_ai_result_host_test.c" \
   -o "$OUT"
"$OUT"
