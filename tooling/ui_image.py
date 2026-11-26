# SPDX-License-Identifier: Unlicense
"""Add a native recovery UI to a verified development recovery image.

The source image's kernel, DTB, recovery_dtbo, deadlines, authentication and
stock AVB metadata are preserved. All outputs belong outside the source tree.
"""

import argparse
import hashlib
import json
from pathlib import Path

from overpressure.api.v1 import avb, bootimg, compress, cpio, stage1

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    """Overlay the native UI while verifying the original boot-image invariants."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=Path, required=True)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--runtime", type=Path, required=True)
    ap.add_argument("--fonts", type=Path, required=True)
    ap.add_argument("--zoneinfo", type=Path, default=Path("/usr/share/zoneinfo"))
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    original = args.base.read_bytes()
    image = bootimg.parse(original)
    runtime = cpio.Tree(nodes=dict(cpio.read_cpio(args.runtime.read_bytes())))
    overlay = cpio.Tree()
    stage1.add_overlay(overlay, stage1.RUNTIME)
    stage1.add_overlay(overlay, ROOT / "initramfs")
    overlay.add_file("usr/bin/pico4-recovery-ui", args.binary.read_bytes(), 0o755)
    overlay.add_symlink("usr/bin/pico4-ui-shot", "pico4-recovery-ui")
    breeze = ROOT / "src/recovery-ui/breeze"
    for name in ("atlas.png", "manifest.json", "COPYING-ICONS", "GPL-3.0.txt", "NOTICE.md"):
        overlay.add_file("usr/share/pico4/recovery/breeze/" + name, (breeze / name).read_bytes())
    for svg in sorted((breeze / "original").rglob("*.svg")):
        overlay.add_file("usr/share/pico4/recovery/breeze/" + svg.relative_to(breeze).as_posix(), svg.read_bytes())
    for zone in ("Asia/Shanghai", "Asia/Tokyo", "Europe/Berlin", "America/New_York"):
        source = (args.zoneinfo / zone).read_bytes()
        if not source.startswith(b"TZif"):
            msg = "Invalid timezone data: " + zone
            raise ValueError(msg)
        overlay.add_file("usr/share/zoneinfo/" + zone, source)
    fonts = ["NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoSansCJK-SC-Regular.otf", "NotoSansCJK-SC-Bold.otf"]
    for name in fonts:
        overlay.add_file("usr/share/pico4/recovery/fonts/" + name, (args.fonts / name).read_bytes())
    for name in ("LICENSE-Noto", "LICENSE-Noto-CJK"):
        overlay.add_file("usr/share/pico4/recovery/fonts/" + name, (args.fonts / name).read_bytes())
    sources = {
        p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((ROOT / "src/recovery-ui").rglob("*"))
        if p.is_file() and p.suffix in (".cpp", ".hpp", ".inc")
    }
    manifest = {
        "schema_version": 1,
        "design": "v2.0",
        "locale": "en",
        "locales": ["zh-CN", "en"],
        "layout_revision": "20261126-weighted-install-progress",
        "breeze_atlas_sha256": hashlib.sha256((breeze / "atlas.png").read_bytes()).hexdigest(),
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "base_sha256": hashlib.sha256(original).hexdigest(),
        "sources": sources,
        "kernel_sha256": hashlib.sha256(image.kernel).hexdigest(),
        "fonts": {name: hashlib.sha256((args.fonts / name).read_bytes()).hexdigest() for name in fonts},
    }
    overlay.add_file("etc/pico4/ui-build.json", json.dumps(manifest, indent=2).encode() + b"\n")
    image.ramdisk += compress.gzip_compress(runtime.to_cpio()) + compress.gzip_compress(overlay.to_cpio())
    packed = avb.with_stock_vbmeta(original, image.pack())
    check = bootimg.parse(packed)
    for field in ("kernel", "dtb", "recovery_dtbo", "full_cmdline"):
        if getattr(check, field) != getattr(bootimg.parse(original), field):
            msg = "image changed " + field
            raise RuntimeError(msg)
    if check.ramdisk != image.ramdisk or len(packed) != len(original):
        msg = "image did not retain the complete ramdisk or original partition size"
        raise RuntimeError(msg)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(packed)
    manifest["image_sha256"] = hashlib.sha256(packed).hexdigest()
    manifest["image_bytes"] = len(packed)
    manifest["initramfs_sources"] = {
        p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((ROOT / "initramfs").rglob("*"))
        if p.is_file()
    }
    manifest["shared_runtime_sources"] = {
        p.relative_to(stage1.RUNTIME).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(stage1.RUNTIME.rglob("*"))
        if p.is_file()
    }
    args.out.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
