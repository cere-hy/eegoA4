#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined \
  -Iinclude src/PushThumb.cpp test/push_thumb_host_test.cpp -o /tmp/pushthumb_host
/tmp/pushthumb_host
