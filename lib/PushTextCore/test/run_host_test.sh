#!/usr/bin/env bash
# 文本推送裁剪纯核 host 断言 — B6.5 S1 判据。
# gcc + ASan/UBSan，无 ESP-IDF / 无硬件。
# 覆盖：BEGIN 编解码 / DATA seq 连续 / END 收全判定 / CRC 不匹配拒绝。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/push_text_core_host_test"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -I"$ROOT/include" \
   "$ROOT/src/push_proto.c" "$ROOT/src/push_session.c" "$ROOT/src/push_sink_core.c" \
   "$HERE/push_text_core_host_test.c" \
   -o "$OUT"
"$OUT"
