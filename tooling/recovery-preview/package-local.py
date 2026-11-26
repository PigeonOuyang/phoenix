#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Package and check an offline Recovery candidate using verified local inputs."""

import argparse
import hashlib
import json
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

from devices.phoenix.kernel import recovery_image
from overpressure.api.v1 import avb, bootimg, compress, cpio


def digest(data: bytes) -> str:
    """Return the SHA-256 digest used by the image and font manifests."""
    return hashlib.sha256(data).hexdigest()


def layers(ramdisk: bytes) -> Iterator[tuple[bytes, dict[str, cpio.Node]]]:
    """Yield the raw archive and entries for each verified gzip member."""
    while ramdisk:
        raw, after, fmt = compress.decompress(ramdisk)
        if fmt != "gzip" or len(after) >= len(ramdisk):
            message = "unexpected ramdisk stream"
            raise ValueError(message)
        yield raw, dict(cpio.read_cpio(raw))
        ramdisk = after


def _prepare_base(args: argparse.Namespace, stock: bytes) -> tuple[Path, bytes]:
    # This build is for offline validation. Production dev image generation supplies its
    # own session key, test-run marker, and instrumentation from hardware_test.prepare_image.
    stock_dtbo = bootimg.parse(stock).recovery_dtbo
    stage1 = bootimg.parse(args.stage1.read_bytes())
    if not stock_dtbo or not stage1.ramdisk:
        message = "missing stage-1 or stock DTBO"
        raise ValueError(message)
    stage1 = stage1.with_cmdline(f"{stage1.full_cmdline} overpressure.part=recovery")
    stage1.recovery_dtbo = stock_dtbo
    seed = args.out / "stage1-recovery-seed.img"
    seed.write_bytes(stage1.pack())
    base = args.out / "stage1-recovery-base.img"
    base.write_bytes(recovery_image.build(args.kernel, args.stock, seed, gpu_firmware=args.firmware))
    return base, stock_dtbo


def _extract_runtime(old_ui_path: Path, font_dir: Path) -> Path:
    font_dir.mkdir(exist_ok=True)
    old_ui = old_ui_path.read_bytes()
    expected = json.loads(old_ui_path.with_suffix(".json").read_text(encoding="utf-8"))
    if digest(old_ui) != expected["image_sha256"]:
        message = "reference UI image hash mismatch"
        raise ValueError(message)
    fonts: dict[str, bytes] = {}
    runtime = None
    for raw, nodes in layers(bootimg.parse(old_ui).ramdisk):
        if any("libcairo.so.2" in name for name in nodes):
            runtime = raw
        for name, node in nodes.items():
            if name.startswith("usr/share/pico4/recovery/fonts/") and node.is_file:
                fonts[Path(name).name] = node.data
    if runtime is None:
        message = "reference lacks the native dependencies"
        raise ValueError(message)
    for name, hash_value in expected["fonts"].items():
        if digest(fonts[name]) != hash_value:
            message = "reference font hash mismatch: " + name
            raise ValueError(message)
    for name in (*expected["fonts"], "LICENSE-Noto", "LICENSE-Noto-CJK"):
        (font_dir / name).write_bytes(fonts[name])
    runtime_path = font_dir.parent / "ui-runtime.cpio"
    runtime_path.write_bytes(runtime)
    return runtime_path


def _verify_package(assembled: bytes, stock: bytes, stock_dtbo: bytes, kernel: Path, binary: Path) -> None:
    image = bootimg.parse(assembled)
    installed: dict[str, cpio.Node] = {}
    for _, nodes in layers(image.ramdisk):
        installed.update(nodes)
    must_have = (
        "init",
        "etc/inittab",
        "etc/overpressure/headset",
        "usr/bin/pico4-install",
        "usr/bin/pico4-recovery-ui",
        "usr/libexec/pico4/task-state",
        "usr/libexec/overpressure/svc",
        "usr/libexec/overpressure/recovery-watch",
    )
    if (
        image.recovery_dtbo != stock_dtbo
        or image.kernel != (kernel / "Image.gz").read_bytes()
        or image.dtb != (kernel / recovery_image.DTB).read_bytes()
        or len(assembled) != len(stock)
        or not all(name in installed for name in must_have)
        or b"svc recovery-ui" not in installed["etc/inittab"].data
        or installed["usr/bin/pico4-recovery-ui"].data != binary.read_bytes()
        or avb.with_stock_vbmeta(stock, image.pack()) != assembled
    ):
        message = "offline Recovery package readback failed"
        raise ValueError(message)


def main() -> None:
    """Build an offline image and verify its payload against the local inputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stage1", "stock", "firmware", "kernel", "old-ui", "binary", "out"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    stock = args.stock.read_bytes()
    base, stock_dtbo = _prepare_base(args, stock)
    font_dir = args.out / "fonts"
    runtime_path = _extract_runtime(args.old_ui, font_dir)
    output = args.out / "recovery-progress-offline.img"
    pack = Path(__file__).resolve().parents[1] / "ui_image.py"
    subprocess.run(  # noqa: S603  # Use this interpreter and the repository's packer with explicit path arguments.
        [
            sys.executable,
            str(pack),
            "--base",
            str(base),
            "--binary",
            str(args.binary),
            "--runtime",
            str(runtime_path),
            "--fonts",
            str(font_dir),
            "--out",
            str(output),
        ],
        check=True,
    )
    assembled = output.read_bytes()
    _verify_package(assembled, stock, stock_dtbo, args.kernel, args.binary)
    print("RECOVERY_OFFLINE_PACKAGE_OK", digest(assembled), len(assembled))


if __name__ == "__main__":
    main()
