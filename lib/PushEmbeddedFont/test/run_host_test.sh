#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
g++ -std=c++17 -Wall -Wextra -Ilib/PushEmbeddedFont/include \
  lib/PushEmbeddedFont/test/pif1_codec_host_test.cpp -o /tmp/pif1_codec_host -lz
/tmp/pif1_codec_host assets/fonts/ui-hanzi.bin
