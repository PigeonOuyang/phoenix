#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
set -euo pipefail
source_root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?usage: test-recovery-clock.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$source_root/src/recovery-ui/clock.cpp" "$source_root/tests/test_recovery_clock.cpp" \
    -o "$output/test-recovery-clock"
"$output/test-recovery-clock"
