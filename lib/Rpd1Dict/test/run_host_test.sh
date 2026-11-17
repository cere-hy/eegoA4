#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TESTDIR=/tmp/rpd1_host_test
rm -rf "$TESTDIR"; mkdir -p "$TESTDIR"

python3 "$DIR/gen_fixture.py" "$TESTDIR/dict.bin"
cp "$ROOT/lib/Rpd1Dict/include/rpd1_core.h" "$TESTDIR/"
cp "$ROOT/lib/Rpd1Dict/src/rpd1_core.c" "$TESTDIR/"

cd "$TESTDIR"
CFLAGS="-std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer"
gcc $CFLAGS -I. rpd1_core.c "$DIR/rpd1_host_test.c" -o rpd1_host_test
./rpd1_host_test dict.bin
