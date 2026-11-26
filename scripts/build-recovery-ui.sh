#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
# Use a verified Alpine aarch64 sysroot from tooling/ui_sysroot.py, not glibc.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "usage: build-recovery-ui.sh ALPINE_SYSROOT OUTPUT_DIRECTORY" >&2
    exit 2
fi
sysroot=$(realpath "$1")
output=$(realpath -m "$2")
source_root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$output"
cxx_include=$(find "$sysroot/usr/include/c++" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)
gcc_library=$(find "$sysroot/usr/lib/gcc/aarch64-alpine-linux-musl" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)
test -n "$cxx_include" && test -f "$gcc_library/crtbeginS.o"
compiler=${CXX:-clang++}
"$compiler" --target=aarch64-linux-musl --sysroot="$sysroot" -fuse-ld=lld \
    -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -pthread \
    -nostdinc++ -isystem "$cxx_include" -isystem "$cxx_include/aarch64-alpine-linux-musl" \
    -isystem "$cxx_include/backward" -I "$sysroot/usr/include/cairo" \
    -I "$sysroot/usr/include/freetype2" -I "$sysroot/usr/include/libdrm" \
    -nostdlib "$sysroot/usr/lib/Scrt1.o" "$sysroot/usr/lib/crti.o" "$gcc_library/crtbeginS.o" \
    "$source_root/src/recovery-ui/main.cpp" "$source_root/src/recovery-ui/app.cpp" \
    "$source_root/src/recovery-ui/backend.cpp" "$source_root/src/recovery-ui/preferences.cpp" \
    "$source_root/src/recovery-ui/clock.cpp" "$source_root/src/recovery-ui/network.cpp" \
    -L "$sysroot/usr/lib" -L "$sysroot/lib" -L "$gcc_library" \
    -Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1 -Wl,--build-id=sha1 \
    -lcairo -lfreetype -ldrm -lpng16 -ljson-c -lstdc++ -lm -lgcc_s -lc \
    "$gcc_library/crtendS.o" "$sysroot/usr/lib/crtn.o" \
    -o "$output/pico4-recovery-ui"
aarch64-linux-gnu-readelf -l "$output/pico4-recovery-ui"
aarch64-linux-gnu-readelf -d "$output/pico4-recovery-ui"
sha256sum "$output/pico4-recovery-ui"
