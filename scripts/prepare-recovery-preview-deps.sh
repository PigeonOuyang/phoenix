#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
# Workspace-local headers for Ubuntu WSLg, with no sudo or apt installation.
set -euo pipefail
out=$(realpath -m "${1:?usage: prepare-recovery-preview-deps.sh OUTPUT_DIRECTORY}")
mkdir -p "$out/debs" "$out/headers"
cd "$out/debs"

for pair in libcairo2-dev:libcairo2 libjson-c-dev:libjson-c5 libdrm-dev:libdrm2 libsdl2-dev:libsdl2-2.0-0; do
    dev=${pair%:*}
    runtime=${pair#*:}
    version=$(dpkg-query -W -f='${Version}' "$runtime")
    metadata=$(apt-cache show "$dev=$version")
    location=$(awk '/^Filename:/{print $2; exit}' <<< "$metadata")
    checksum=$(awk '/^SHA256:/{print $2; exit}' <<< "$metadata")
    [[ -n $location && $checksum =~ ^[a-f0-9]{64}$ ]]
    file=${location##*/}
    if [[ ! -f $file ]] || ! printf '%s  %s\n' "$checksum" "$file" | sha256sum --status --check; then
        curl --fail --silent --show-error --location --retry 2 \
            "https://archive.ubuntu.com/ubuntu/$location" --output "$file"
    fi
    printf '%s  %s\n' "$checksum" "$file" | sha256sum --check
    dpkg-deb --extract "$file" "$out/headers"
done
