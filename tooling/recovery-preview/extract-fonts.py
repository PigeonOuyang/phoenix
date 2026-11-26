#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Copy only the four renderer fonts from an existing local Recovery image."""

import argparse
import hashlib
import json
from pathlib import Path

from overpressure.api.v1 import bootimg, compress, cpio

FONTS = (
    "NotoSans-Regular.ttf",
    "NotoSans-Bold.ttf",
    "NotoSansCJK-SC-Regular.otf",
    "NotoSansCJK-SC-Bold.otf",
)
PREFIX = "usr/share/pico4/recovery/fonts/"


def main() -> None:
    """Verify the image's font manifest before extracting the renderer fonts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    archive = bootimg.parse(args.image.read_bytes()).ramdisk
    found: dict[str, bytes] = {}
    expected: dict[str, str] = {}
    while archive:
        payload, archive, _ = compress.decompress(archive)
        for member, node in cpio.read_cpio(payload):
            name = member.removeprefix("./")
            if name == "etc/pico4/ui-build.json" and node.is_file:
                expected = json.loads(node.data)["fonts"]
            if name.startswith(PREFIX) and name[len(PREFIX) :] in FONTS and node.is_file:
                found[name[len(PREFIX) :]] = node.data
    if set(found) != set(FONTS):
        parser.error("the existing Recovery image does not contain all four renderer fonts")
    if not all(hashlib.sha256(found[name]).hexdigest() == expected.get(name) for name in FONTS):
        parser.error("renderer fonts do not match the image's build manifest")
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in found.items():
        (args.output / name).write_bytes(data)
    print(f"Verified and extracted {len(found)} native font files")


if __name__ == "__main__":
    main()
