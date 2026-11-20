#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
g++ -std=c++17 -Wall -Wextra -Ilib/GfxRenderer \
  lib/GfxRenderer/test/ui_fallback_chain_host_test.cpp -o /tmp/ui_fallback_chain_host
/tmp/ui_fallback_chain_host
