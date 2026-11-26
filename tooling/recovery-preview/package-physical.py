#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Prepare an authenticated Recovery-only hardware acceptance image."""

import argparse
import hashlib
import json
import re
from pathlib import Path

from devices.phoenix.kernel import recovery_image
from overpressure import sparse
from overpressure.api.v1 import avb, bootimg, compress, cpio, stage1


def sha(data: bytes) -> str:
    """Return the SHA-256 digest used by the hardware acceptance receipt."""
    return hashlib.sha256(data).hexdigest()


def _overlay(phoenix: Path, key: bytes, binary: bytes) -> tuple[cpio.Tree, bytes]:
    tree = cpio.Tree()
    stage1.add_overlay(tree, phoenix / "initramfs")
    tree.mkdir("root", 0o700)
    tree.mkdir("root/.ssh", 0o700)
    tree.add_file("root/.ssh/authorized_keys", key + b"\n", 0o600)
    svc_path = "usr/libexec/overpressure/svc"
    svc = (stage1.RUNTIME / svc_path).read_bytes().replace(b"\r\n", b"\n")
    passwordless = b"exec dropbear -F -E -R -B -p 22 -W 4194304"
    if svc.count(passwordless) != 1:
        message = "stage-1 SSH service did not match the offline image"
        raise ValueError(message)
    tree.add_file(svc_path, svc.replace(passwordless, passwordless.replace(b"-B", b"-s")), 0o755)
    tree.add_file("usr/bin/pico4-recovery-ui", binary, 0o755)
    return tree, svc


def _repack(original: bootimg.BootImage, stock: bytes, tree: cpio.Tree, run: str) -> tuple[bytes, bootimg.BootImage]:
    words = [word for word in original.full_cmdline.split() if not word.startswith("overpressure.test_run=")]
    image = original.with_cmdline(" ".join([*words, "overpressure.test_run=" + run]))
    image.ramdisk += compress.gzip_compress(tree.to_cpio())
    packed = avb.with_stock_vbmeta(stock, image.pack())
    actual = bootimg.parse(packed)
    if (
        actual.kernel != original.kernel
        or actual.dtb != original.dtb
        or actual.recovery_dtbo != original.recovery_dtbo
        or actual.ramdisk != image.ramdisk
        or len(packed) != len(stock)
    ):
        message = "verified Recovery payload changed while repacking"
        raise ValueError(message)
    return packed, actual


def main() -> None:
    """Package and structurally verify the Recovery and installation boot images."""
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "stock", "binary", "public-key", "kernel", "out"):
        ap.add_argument(f"--{name}", type=Path, required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    base, stock, binary = a.base.read_bytes(), a.stock.read_bytes(), a.binary.read_bytes()
    receipt = json.loads(a.base.with_suffix(".json").read_text(encoding="utf-8"))
    if sha(base) != receipt["image_sha256"] or len(base) != len(stock):
        message = "offline image or stock partition size mismatch"
        raise ValueError(message)
    key = a.public_key.read_bytes().strip()
    if not re.fullmatch(rb"ssh-ed25519 [a-zA-Z0-9+/=]+(?: [^\r\n]+)?", key):
        message = "expected one Ed25519 public key"
        raise ValueError(message)
    image = bootimg.parse(base)
    phoenix = Path(__file__).resolve().parents[2]
    tree, svc = _overlay(phoenix, key, binary)
    run = sha(tree.to_cpio())[:24]
    # The authenticated dev loop verifies the live run marker and device identity.
    for path, value in (("test-run", run), ("test-connect-timeout", "600"), ("test-timeout", "3600")):
        tree.add_file("etc/overpressure/" + path, (value + "\n").encode())
    receipt["base_ui_sources"] = receipt.pop("sources")
    receipt["shared_runtime_sources"]["usr/libexec/overpressure/svc"] = sha(svc)
    source_dir = Path(__file__).resolve().parents[2] / "src/recovery-ui"
    receipt["sources"] = {
        f"src/recovery-ui/{path.relative_to(source_dir).as_posix()}": sha(path.read_bytes())
        for path in sorted(source_dir.rglob("*"))
        if path.is_file() and path.suffix in (".cpp", ".hpp", ".inc")
    }
    receipt["initramfs_sources"] = {
        path.relative_to(phoenix).as_posix(): sha(path.read_bytes())
        for path in sorted((phoenix / "initramfs").rglob("*"))
        if path.is_file()
    }
    receipt.update(binary_sha256=sha(binary), physical_test_id=run, simulation_only=False)
    tree.add_file("etc/pico4/ui-build.json", json.dumps(receipt, indent=2).encode() + b"\n")
    packed, actual = _repack(image, stock, tree, run)
    recovery = a.out / "dev-recovery.img"
    recovery.write_bytes(packed)
    transport = sparse.encode(packed)
    recovery.with_suffix(".simg").write_bytes(transport)
    boot = recovery_image.build(a.kernel, a.stock, recovery, direct=True, boot=True)
    boot_image = bootimg.parse(boot)
    if boot_image.ramdisk or boot_image.recovery_dtbo or boot_image.kernel != actual.kernel:
        message = "installation boot image failed structural verification"
        raise ValueError(message)
    (a.out / "valve-boot.img").write_bytes(boot)
    result = {
        "recovery_kernel": "valve",
        "recovery_sha256": sha(packed),
        "recovery_size": len(packed),
        "sparse_sha256": sha(transport),
        "binary_sha256": sha(binary),
        "test_run": run,
        "boot_sha256": sha(boot),
        "simulation_only": False,
    }
    (a.out / "dev-image.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
