#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
g++ -std=c++17 -Wall -Wextra -Ilib/miniz/third_party \
  lib/PushEmbeddedFont/test/stack_budget_host_test.cpp -o /tmp/stack_budget_host
/tmp/stack_budget_host lib/PushEmbeddedFont/src/PushEmbeddedFont.cpp src/activities/ActivityManager.cpp
