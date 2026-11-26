#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
set -euo pipefail
source_root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?usage: test-recovery-ui.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$source_root/src/recovery-ui/app.cpp" "$source_root/src/recovery-ui/test_app.cpp" \
    "$source_root/src/recovery-ui/clock.cpp" \
    -o "$output/test-recovery-ui"
"$output/test-recovery-ui"
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I "$source_root/src/recovery-ui" "$source_root/tests/test_install_log_locale.cpp" \
    -o "$output/test-install-log-locale"
"$output/test-install-log-locale"
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I "$source_root/src/recovery-ui" "$source_root/tests/test_recovery_log_progress.cpp" \
    "$source_root/src/recovery-ui/app.cpp" "$source_root/src/recovery-ui/clock.cpp" \
    "$source_root/tooling/recovery-preview/fixture-backend.cpp" \
    -o "$output/test-recovery-log-progress"
"$output/test-recovery-log-progress"
