# SPDX-License-Identifier: Unlicense
"""Native UI device checks through the existing authenticated development loop."""

import argparse
import hashlib
import io
import json
import re
import shlex
import shutil
import subprocess
import tarfile
from pathlib import Path

from ui_checkout import load  # ty: ignore[import-not-found]  # Direct script execution puts this directory on sys.path.

load()
from devices.phoenix.tooling import localconfig  # noqa: E402  # Import only after loading this checkout.
from devices.phoenix.tooling.devloop import Dev  # noqa: E402  # Import only after loading this checkout.
from overpressure.api.v1 import fastboot  # noqa: E402  # Bind this checkout before importing headset tooling.


def main() -> None:  # noqa: C901, PLR0912, PLR0915  # One branch per existing authenticated device action.
    """Run the requested native UI action through the authenticated dev loop."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--backup", type=Path, required=True)
    ap.add_argument("--identity", type=Path, required=True)
    ap.add_argument(
        "action",
        choices=[
            "read",
            "reconnect",
            "deploy",
            "screenshot",
            "reboot",
            "bootloader-info",
            "upload",
            "gallery",
            "time-sync",
        ],
    )
    ap.add_argument("--image", type=Path)
    ap.add_argument("--destination", type=Path)
    ap.add_argument(
        "--left-eye-only",
        action="store_true",
        help="download only the 2160 × 2160 left-eye frame",  # noqa: RUF001  # Pixel dimension separator.
    )
    ap.add_argument("--source", type=Path)
    ap.add_argument("--remote")
    ap.add_argument(
        "--command",
        default="cat /run/overpressure/ui-frame.json /run/overpressure/svc-recovery-ui.log; uname -r",
    )
    args = ap.parse_args()
    settings = localconfig.load(args.env, {})
    settings.backup = str(args.backup)
    settings.identity = str(args.identity)
    dev = Dev(settings, args.work, out=args.out)
    if args.action == "bootloader-info":
        print(dev.fastboot("oem", "device-info"))
    elif args.action == "read":
        print(dev.ssh(args.command, 60, check=False))
    elif args.action == "reconnect":
        print("boot_partition=" + dev.connect(flashed=False))
        print(dev.ssh(args.command, 60, check=False))
    elif args.action == "time-sync":
        print(json.dumps(dev.sync_time(), ensure_ascii=False))
    elif args.action == "upload":
        if not args.source or not args.remote or not re.fullmatch(r"/run/overpressure/[A-Za-z0-9_.-]+", args.remote):
            ap.error("upload requires --source and a single file under /run/overpressure/")
        # Development staging remains in volatile memory.
        command = "umask 077; cat > " + shlex.quote(args.remote)
        with args.source.open("rb") as stream:
            subprocess.run(  # noqa: S603  # Authenticated SSH argv; the validated remote path is shell-quoted.
                dev.ssh_argv(command), stdin=stream, check=True, timeout=90
            )
        digest = dev.ssh("sha256sum " + shlex.quote(args.remote)).split()[0]
        if digest != hashlib.sha256(args.source.read_bytes()).hexdigest():
            msg = "uploaded file hash mismatch"
            raise RuntimeError(msg)
        print("upload_sha256=" + digest)
    elif args.action == "deploy":
        if not args.image:
            ap.error("deploy requires --image")
        dev.to_fastboot()
        authority = dev.fastboot("oem", "device-info")
        if not re.search(r"\b(?:DeviceUnlocked|Device unlocked)\s*[:=]\s*true\b", authority):
            msg = "stock bootloader did not attest DeviceUnlocked=true"
            raise RuntimeError(msg)
        dev.check_boot_partition(args.image, "recovery")
        fastboot.flash(dev.fastboot, "recovery", args.image, sparse_path=args.work / "ui-recovery.simg")
        shutil.copyfile(args.image, args.work / "dev-recovery.img")
        info = json.loads((args.work / "dev-image.json").read_text())
        info["recovery_sha256"] = hashlib.sha256(args.image.read_bytes()).hexdigest()
        (args.work / "dev-image.json").write_text(json.dumps(info, indent=2) + "\n")
        dev.fastboot("reboot", "recovery")
        print("boot_partition=" + dev.connect(flashed=True))
    elif args.action == "gallery":
        if not args.destination:
            ap.error("gallery requires --destination")
        renderer = args.remote or "pico4-recovery-ui"
        if args.remote and not re.fullmatch(r"/run/overpressure/[A-Za-z0-9_.-]+", args.remote):
            ap.error("gallery renderer must be a single file under /run/overpressure/")
        if args.remote:
            print(dev.ssh("chmod 700 " + shlex.quote(renderer)))
        print(dev.ssh(shlex.quote(renderer) + " --render-gallery /run/overpressure/ui-gallery", 90))
        capture = subprocess.run(  # noqa: S603  # Authenticated SSH argv with a fixed volatile gallery path.
            dev.ssh_argv("tar -czf - -C /run/overpressure/ui-gallery ."), stdout=subprocess.PIPE, check=True, timeout=60
        )
        args.destination.mkdir(parents=True, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(capture.stdout), mode="r:gz") as archive:
            archive.extractall(args.destination, filter="data")
        print("gallery=offline_layout_check")
    elif args.action == "reboot":
        old = dev.ssh("cat /proc/sys/kernel/random/boot_id").strip()
        dev.ssh_reboot("pico4-reboot recovery")
        dev.reach(180)
        new = dev.ssh("cat /proc/sys/kernel/random/boot_id").strip()
        if old == new:
            msg = "device boot id did not change"
            raise RuntimeError(msg)
        dev.sync_time()
        print("device_reboot_verified=true")
        print(dev.ssh("cat /run/overpressure/ui-frame.json; cat /run/overpressure/svc-recovery-ui.log", 30))
    else:
        if not args.destination:
            ap.error("screenshot requires --destination")
        record = json.loads(dev.ssh("pico4-ui-shot", 60))
        remote = record["directory"]
        if not record.get("atomic_commit_completed") or not re.fullmatch(
            r"/run/overpressure/screenshots/frame-[0-9]+-[0-9]+", remote
        ):
            msg = "invalid screenshot receipt"
            raise RuntimeError(msg)
        args.destination.mkdir(parents=True, exist_ok=True)
        # Use the authenticated ssh argv for binary transport; no extra remote UI protocol.
        frames = ("左眼.png",) if args.left_eye_only else ("双眼.png", "左眼.png", "右眼.png", "截图信息.json")
        for name in frames:
            with (args.destination / name).open("wb") as stream:
                command = "cat " + shlex.quote(remote + "/" + name)
                run = subprocess.run(  # noqa: S603  # Authenticated SSH argv with a validated, quoted screenshot path.
                    dev.ssh_argv(command), stdout=stream, stderr=subprocess.PIPE, timeout=60, check=False
                )
                if run.returncode:
                    msg = run.stderr.decode(errors="replace")
                    raise RuntimeError(msg)
        print(
            json.dumps(
                {"directory": str(args.destination), "frame": record["frame"], "page": record["page"]},
                ensure_ascii=False,
            )
        )


if __name__ == "__main__":
    main()
