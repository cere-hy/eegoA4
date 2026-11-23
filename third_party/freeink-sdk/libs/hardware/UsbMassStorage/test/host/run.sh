#!/usr/bin/env bash
# S3cPF7 P0 — host regression for the MSC callback I/O-lock RAII guard.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
INC="$HERE/../../include"
OUT="${TMPDIR:-/tmp}/msc_io_lock_test"
g++ -std=c++17 -O2 -pthread -I"$INC" "$HERE/test_msc_io_lock.cpp" -o "$OUT"
exec "$OUT"
