#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
# Every mount/ioctl/rename is wrapped; this suite never accesses real partitions.
set -euo pipefail
source_root=$(cd "$(dirname "$0")/.." && pwd)
output=$(realpath -m "${1:?usage: test-recovery-preferences.sh OUTPUT_DIRECTORY [JSONC_INCLUDE]}")
mkdir -p "$output"
include=()
if [[ $# == 2 ]]; then include=(-I "$2"); fi
${CXX:-c++} -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I "$source_root/src/recovery-ui" "${include[@]}" \
    "$source_root/tests/test_recovery_preferences.cpp" \
    -Wl,--wrap=open,--wrap=openat,--wrap=close,--wrap=dup,--wrap=fstat,--wrap=fstatfs \
    -Wl,--wrap=realpath,--wrap=ioctl,--wrap=mount,--wrap=umount2,--wrap=mkdirat \
    -Wl,--wrap=fsync,--wrap=renameat,--wrap=syscall,--wrap=execve \
    -l:libjson-c.so.5 -o "$output/preferences-test"
cd "$output"
./preferences-test
