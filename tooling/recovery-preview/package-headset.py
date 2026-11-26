#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Add the on-headset rehearsal to a previously verified Recovery image."""

import argparse
import hashlib
import json
import re
from collections.abc import Iterator
from pathlib import Path

from overpressure import sparse
from overpressure.api.v1 import avb, bootimg, compress, cpio

PREVIEW = "usr/share/pico4/recovery/preview"
MAX_TASK_BYTES = 65536
SERVICE = """#!/bin/sh
set -eu
case "${1:-}" in --timeout-preferences) exit 1 ;; esac
umask 077
preview=/run/overpressure/ui-install-preview
mkdir -p "$preview"
if [ ! -f "$preview/active" ]; then
	cp /usr/share/pico4/recovery/preview/* "$preview/"
	: > "$preview/active"
fi
exec /usr/bin/pico4-recovery-ui
"""


def digest(blob: bytes) -> str:
    """Return the SHA-256 digest used by the preview receipt."""
    return hashlib.sha256(blob).hexdigest()


def entries(ramdisk: bytes) -> Iterator[tuple[str, cpio.Node]]:
    """Yield entries from each verified gzip member of the ramdisk."""
    while ramdisk:
        raw, rest, fmt = compress.decompress(ramdisk)
        if fmt != "gzip" or len(rest) >= len(ramdisk):
            message = "unrecognized ramdisk member"
            raise ValueError(message)
        yield from cpio.read_cpio(raw)
        ramdisk = rest


def _load_base(args: argparse.Namespace) -> tuple[bytes, bytes, bootimg.BootImage, dict[str, cpio.Node]]:
    base, stock = args.base.read_bytes(), args.stock.read_bytes()
    expected = json.loads(args.base.with_suffix(".json").read_text(encoding="utf-8"))
    if digest(base) != expected["image_sha256"] or len(base) != len(stock):
        message = "Recovery base does not match its manifest or partition size"
        raise ValueError(message)
    image = bootimg.parse(base)
    if "overpressure.part=recovery" not in image.full_cmdline.split() or "overpressure.rw=1" in image.full_cmdline:
        message = "base must be read-only Recovery"
        raise ValueError(message)
    installed = dict(entries(image.ramdisk))
    return base, stock, image, installed


def _load_payload(args: argparse.Namespace) -> tuple[bytes, bytes]:
    binary = args.binary.read_bytes()
    if binary[:6] != b"\x7fELF\x02\x01" or binary[18:20] != b"\xb7\0":
        message = "renderer is not a Linux ARM64 ELF"
        raise ValueError(message)
    key = args.public_key.read_bytes().strip()
    if not re.fullmatch(rb"ssh-ed25519 [A-Za-z0-9+/=]+(?: [^\r\n]+)?", key):
        message = "expected one public SSH key"
        raise ValueError(message)
    return binary, key


def _preview_tree(fixtures: Path) -> tuple[cpio.Tree, int, int]:
    tree = cpio.Tree()
    for member in sorted(fixtures.iterdir()):
        if member.name in {"timeline", "initial", "boot.log"} or re.fullmatch(r"task\.[0-9]+\.(json|log)", member.name):
            tree.add_file(f"{PREVIEW}/{member.name}", member.read_bytes(), 0o644)
    frame_count = len((fixtures / "timeline").read_text(encoding="ascii").splitlines())
    for index in range(frame_count):
        task = tree.nodes[f"{PREVIEW}/task.{index}.json"].data
        if len(task) > MAX_TASK_BYTES or not isinstance(json.loads(task)["stages"], list):
            message = f"invalid replay frame {index}"
            raise ValueError(message)
        if f"{PREVIEW}/task.{index}.log" not in tree.nodes:
            message = f"missing replay log {index}"
            raise ValueError(message)
    initial = int((fixtures / "initial").read_text(encoding="ascii"))
    start = json.loads(tree.nodes[f"{PREVIEW}/task.{initial}.json"].data)
    if start["stages"][start["active"]]["id"] != "build":
        message = "replay must open at the build stage"
        raise ValueError(message)
    if not any(
        s["id"] == "package-install" and s["status"] == "running" and s["done"] > 0
        for s in start["stages"][start["active"]]["substeps"]
    ):
        message = "initial package installation sample missing"
        raise ValueError(message)
    return tree, frame_count, initial


def _add_services(tree: cpio.Tree, installed: dict[str, cpio.Node], binary: bytes, key: bytes) -> None:
    tree.add_file("usr/bin/pico4-recovery-ui", binary, 0o755)
    tree.add_file("usr/libexec/pico4/recovery-preview", SERVICE.encode(), 0o755)
    config = installed["etc/overpressure/headset"].data
    ui = b"OP_RECOVERY_UI=/usr/bin/pico4-recovery-ui"
    if config.count(ui) != 1:
        message = "missing base Recovery UI declaration"
        raise ValueError(message)
    tree.add_file("etc/overpressure/headset", config.replace(ui, b"OP_RECOVERY_UI=/usr/libexec/pico4/recovery-preview"))
    svc = installed["usr/libexec/overpressure/svc"].data
    dropbear = b"exec dropbear -F -E -R -B -p 22 -W 4194304"
    if svc.count(dropbear) != 1:
        message = "could not pin the rehearsal SSH service to public keys"
        raise ValueError(message)
    tree.add_file("usr/libexec/overpressure/svc", svc.replace(dropbear, dropbear.replace(b"-B", b"-s")), 0o755)
    tree.mkdir("root", 0o700)
    tree.mkdir("root/.ssh", 0o700)
    tree.add_file("root/.ssh/authorized_keys", key + b"\n", 0o600)


def _pack(base: bytes, stock: bytes, image: bootimg.BootImage, tree: cpio.Tree, preview_id: str) -> bytes:
    command = [word for word in image.full_cmdline.split() if not word.startswith("overpressure.autoreboot")]
    command += ["overpressure.autoreboot=0", "overpressure.preview_id=" + preview_id]
    original = bootimg.parse(base)
    image = image.with_cmdline(" ".join(command))
    image.ramdisk += compress.gzip_compress(tree.to_cpio())
    packed = avb.with_stock_vbmeta(stock, image.pack())
    actual = bootimg.parse(packed)
    if (
        len(packed) != len(stock)
        or actual.ramdisk != image.ramdisk
        or actual.kernel != original.kernel
        or actual.dtb != original.dtb
        or actual.recovery_dtbo != bootimg.parse(stock).recovery_dtbo
        or actual.full_cmdline != " ".join(command)
    ):
        message = "headset preview image failed offline verification"
        raise ValueError(message)
    return packed


def main() -> None:
    """Validate the replay fixtures and package an authenticated preview image."""
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "stock", "binary", "fixtures", "public-key", "out"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    args = parser.parse_args()
    base, stock, image, installed = _load_base(args)
    binary, key = _load_payload(args)
    tree, frame_count, initial = _preview_tree(args.fixtures)
    _add_services(tree, installed, binary, key)
    preview_id = digest(tree.to_cpio())[:16]
    record = {
        "base_sha256": digest(base),
        "binary_sha256": digest(binary),
        "preview_id": preview_id,
        "frame_count": frame_count,
        "initial_frame": initial,
        "partition": "recovery",
        "simulation_only": True,
    }
    info = json.loads(installed["etc/pico4/ui-build.json"].data)
    info.update(record)
    info["layout_revision"] += "-headset-preview"
    tree.add_file("etc/pico4/ui-build.json", json.dumps(info, indent=2).encode() + b"\n")
    packed = _pack(base, stock, image, tree, preview_id)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(packed)
    transport = sparse.encode(packed)
    args.out.with_suffix(".simg").write_bytes(transport)
    record.update(
        image_sha256=digest(packed),
        image_bytes=len(packed),
        sparse_sha256=digest(transport),
        sparse_bytes=len(transport),
    )
    args.out.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n", encoding="ascii")
    print(json.dumps(record))


if __name__ == "__main__":
    main()
