# SPDX-License-Identifier: Unlicense
"""Fetch complete, authenticated CJK fonts rather than design-only subsets."""

import argparse
import hashlib
import io
import json
from pathlib import Path

from fontTools.ttLib import TTCollection, TTFont  # ty: ignore[import-not-found]  # Optional font extraction tooling.

from overpressure import alpine, cpio


def main() -> None:
    """Extract complete Simplified Chinese faces from authenticated Alpine fonts."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--mirror", default="https://mirrors.tuna.tsinghua.edu.cn/alpine")
    ap.add_argument("--inspect", action="store_true")
    args = ap.parse_args()
    alpine.MIRROR = args.mirror.rstrip("/")
    index = alpine.load_index(str(args.cache), "v3.24", ["main", "community"], "aarch64")
    package = index.packages["font-noto-cjk"]
    tree = cpio.Tree()
    alpine.unpack_tar(tree, alpine.fetch_package(package, str(args.cache)))
    candidates = {name: node for name, node in tree.nodes.items() if name.endswith((".ttc", ".otf", ".ttf"))}
    print(json.dumps({"package": package.filename, "fonts": sorted(candidates)}, indent=2))
    if args.inspect:
        return
    args.out.mkdir(parents=True, exist_ok=True)
    records = []
    for weight in ("Regular", "Bold"):
        matches = [(name, node) for name, node in candidates.items() if name.endswith(f"NotoSansCJK-{weight}.ttc")]
        if len(matches) != 1:
            msg = f"expected exactly one {weight} Sans CJK collection"
            raise RuntimeError(msg)
        name, node = matches[0]
        collection = TTCollection(io.BytesIO(node.data))
        faces = [font for font in collection.fonts if font["name"].getDebugName(6) == f"NotoSansCJKsc-{weight}"]
        if len(faces) != 1:
            msg = f"SC face not uniquely found in {name}"
            raise RuntimeError(msg)
        target = args.out / f"NotoSansCJK-SC-{weight}.otf"
        faces[0].save(target)
        font = TTFont(target)
        cmap = font.getBestCmap()
        if not cmap or not all(ord(ch) in cmap for ch in "语言唯首次选择简体中文"):
            msg = "complete Chinese glyph coverage check failed"
            raise RuntimeError(msg)
        records.append(
            {
                "source": name,
                "name": target.name,
                "source_sha256": hashlib.sha256(node.data).hexdigest(),
                "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "glyph_count": len(cmap),
            }
        )
    (args.out / "cjk-fonts.json").write_text(
        json.dumps({"package": package.filename, "checksum": package.checksum, "fonts": records}, indent=2) + "\n"
    )


if __name__ == "__main__":
    main()
