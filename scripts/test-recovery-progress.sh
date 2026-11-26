#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=()
if [[ -n ${PICO4_TEST_JSON_HEADERS:-} ]]; then
    flags=(-I "$PICO4_TEST_JSON_HEADERS")
fi
if [[ -n ${PICO4_TEST_JSON_LIB:-} ]]; then
    flags+=("$PICO4_TEST_JSON_LIB")
else
    read -ra json_flags <<< "$(pkg-config --cflags --libs json-c)"
    flags+=("${json_flags[@]}")
fi
${CXX:-c++} -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -ffunction-sections -fdata-sections \
    -I "$root/src/recovery-ui" "$root/tests/test_recovery_progress.cpp" "$root/src/recovery-ui/backend.cpp" \
    -Wl,--gc-sections "${flags[@]}" -o "$out/test-progress"
"$out/test-progress"
