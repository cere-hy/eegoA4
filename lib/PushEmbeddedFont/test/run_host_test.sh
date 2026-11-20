#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
g++ -std=c++17 -Wall -Wextra -Ilib/PushEmbeddedFont/include \
  lib/PushEmbeddedFont/test/pif1_codec_host_test.cpp -o /tmp/pif1_codec_host -lz
/tmp/pif1_codec_host assets/fonts/ui-hanzi.bin

# S3cPF5: confirmed-absent codepoint cache semantics
g++ -std=c++17 -Wall -Wextra -Ilib/PushEmbeddedFont/include -Ilib/EpdFont \
  lib/PushEmbeddedFont/test/missing_cache_host_test.cpp -o /tmp/pf5_missing_cache_host
/tmp/pf5_missing_cache_host
