#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
ui="$root/src/recovery-ui"
preview="$root/tooling/recovery-preview"
out=$(realpath -m "${1:?usage: build-recovery-preview.sh OUTPUT_DIRECTORY [MIRRORED_HEADERS] [--accept-only]}")
headers=${2:-}
mode=${3:-}
mkdir -p "$out"

if [[ -n $headers && $headers != - ]]; then
    include="$headers/usr/include"
    flags=$(pkg-config --cflags --libs freetype2 libpng)
    read -ra ft_png <<< "$flags"
    json_flags=(-isystem "$include" -l:libjson-c.so.5)
    ui_flags=(-isystem "$include/$(gcc -print-multiarch)" -isystem "$include/libdrm" -isystem "$include/SDL2"
              -l:libcairo.so.2 -l:libdrm.so.2 -l:libSDL2-2.0.so.0 "${ft_png[@]}")
else
    flags=$(pkg-config --cflags --libs json-c)
    read -ra json_flags <<< "$flags"
    ui_flags=()
    if [[ $mode != --accept-only ]]; then
        flags=$(pkg-config --cflags --libs cairo freetype2 libdrm libpng sdl2)
        read -ra ui_flags <<< "$flags"
    fi
fi

common=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -ffunction-sections -fdata-sections
        '-Wl,--gc-sections' -I"$ui" -I"$preview")
${CXX:-c++} "${common[@]}" "$preview/accept.cpp" "$preview/replay.cpp" "$ui/backend.cpp" \
    "${json_flags[@]}" -o "$out/accept-recovery-preview"
if [[ $mode == --accept-only ]]; then exit 0; fi

${CXX:-c++} "${common[@]}" "$preview/window.cpp" "$preview/render-check.cpp" \
    "$preview/replay.cpp" "$preview/fixture-backend.cpp" \
    "$preview/painter-bridge.cpp" "$ui/app.cpp" "$ui/clock.cpp" \
    "${json_flags[@]}" "${ui_flags[@]}" -o "$out/recovery-preview-build"
# The private painter currently lives with the device entry point. Only the
# painter adapter may survive in the interactive executable.
symbols=$(nm -C "$out/recovery-preview-build")
if grep -Eq 'preview_unlinked_recovery_main|screenshot_client|Display::commit|drmModeAtomicCommit' <<< "$symbols"; then
    echo "Device-side code unexpectedly retained in the preview" >&2
    exit 1
fi
mv -f "$out/recovery-preview-build" "$out/recovery-preview"
