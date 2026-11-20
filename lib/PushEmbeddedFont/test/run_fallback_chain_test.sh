#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
g++ -std=c++17 -Wall -Wextra -Ilib/PushEmbeddedFont/include \
  lib/PushEmbeddedFont/test/fallback_chain_host_test.cpp -o /tmp/fallback_chain_host
/tmp/fallback_chain_host
