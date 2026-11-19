#!/usr/bin/env bash
# S3c-final P0#3 — host regression for the ordered advance table (crash fix).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/advance_table_test"
g++ -std=c++17 -O2 -pthread \
  -I"$ROOT/lib/EpdFont" \
  "$ROOT/lib/EpdFont/test/advance_table_host_test.cpp" -o "$OUT"
exec "$OUT"
