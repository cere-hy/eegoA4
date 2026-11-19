#!/usr/bin/env bash
# PushTextFilter host 断言 — B6.5 S3 判据。gcc + ASan/UBSan，无 ESP-IDF/硬件。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/push_text_filter_host_test"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -I"$ROOT/include" \
   "$ROOT/src/PushTextFilter.c" \
   "$HERE/push_text_filter_host_test.c" \
   -o "$OUT"
"$OUT"
