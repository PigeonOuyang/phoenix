#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
set -euo pipefail
source_root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?usage: test-recovery-network.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
exec > >(tee "$output/test-recovery-network.log") 2>&1
set -x
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$source_root/src/recovery-ui/network.cpp" "$source_root/tests/test_recovery_network.cpp" \
    -o "$output/test-recovery-network"
"$output/test-recovery-network"
