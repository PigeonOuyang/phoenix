# SPDX-License-Identifier: Unlicense
"""Verified Alpine aarch64 dependency bundle for the native recovery UI.

Run in an overpressure checkout, passing an output directory outside the source
tree. Package signatures/checksums use the same resolver as the recovery image.
"""

import argparse
import json
import shutil
from pathlib import Path

from overpressure import alpine, cpio

RUNTIME = ["cairo", "freetype", "libdrm", "libpng", "json-c", "libstdc++"]
DEVEL = ["cairo-dev", "freetype-dev", "libdrm-dev", "libpng-dev", "json-c-dev", "musl-dev", "g++"]


def main() -> None:
    """Bundle authenticated build and runtime packages for the native UI."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cache", type=Path, required=True)
    ap.add_argument("--mirror", default=alpine.MIRROR)
    ap.add_argument("--fonts", type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    alpine.MIRROR = args.mirror.rstrip("/")
    index = alpine.load_index(str(args.cache), "v3.24", ["main", "community"], "aarch64")
    packages = alpine.resolve(index, DEVEL, [])
    tree = cpio.Tree()
    records = []
    for pkg in packages:
        alpine.unpack_tar(tree, alpine.fetch_package(pkg, str(args.cache)))
        records.append({"name": pkg.name, "version": pkg.version, "checksum": pkg.checksum})
    (args.out / "sysroot.cpio").write_bytes(tree.to_cpio())
    runtime = cpio.Tree()
    for pkg in alpine.resolve(index, RUNTIME, []):
        alpine.unpack_tar(runtime, alpine.fetch_package(pkg, str(args.cache)))
    (args.out / "runtime.cpio").write_bytes(runtime.to_cpio())
    (args.out / "packages.json").write_text(json.dumps(records, indent=2) + "\n")
    if args.fonts:
        dest = args.out / "fonts"
        dest.mkdir(exist_ok=True)
        for weight in ("Regular", "Bold"):
            shutil.copyfile(args.fonts / f"NotoSans-{weight}.ttf", dest / f"NotoSans-{weight}.ttf")
            # Design WOFF subsets cannot cover new native strings or long logs.
            shutil.copyfile(args.fonts / f"NotoSansCJK-SC-{weight}.otf", dest / f"NotoSansCJK-SC-{weight}.otf")
        for name in ("LICENSE-Noto", "LICENSE-Noto-CJK"):
            shutil.copyfile(args.fonts / name, dest / name)


if __name__ == "__main__":
    main()
