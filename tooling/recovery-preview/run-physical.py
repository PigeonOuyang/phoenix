#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Run the authorized acceptance lanes without issuing a normal boot."""

import argparse
import json
import os
import re
import time
from pathlib import Path
from typing import Any

from devices.phoenix.tooling import devloop, localconfig
from overpressure.api.v1 import qualcomm

GUARD_FIELDS = 5


def guard(dev: devloop.Dev, receipt: dict[str, Any]) -> str:
    """Verify the Recovery boot identity and return its boot ID."""
    out = dev.ssh(
        "cat /proc/1/comm; grep -ow overpressure.part=recovery /proc/cmdline; "
        "head -c 13 /dev/block/by-name/misc; echo; cat /proc/sys/kernel/random/boot_id; "
        "cat /etc/overpressure/test-run",
        15,
    ).splitlines()
    if (
        len(out) != GUARD_FIELDS
        or not out[0].startswith("overpressure-in")
        or out[1:3] != ["overpressure.part=recovery", "boot-recovery"]
        or not re.fullmatch(r"[0-9a-f-]{36}", out[3])
        or out[4] != receipt["test_run"]
    ):
        message = "not in the authorized Recovery boot"
        raise RuntimeError(message)
    return out[3]


def _probe(dev: devloop.Dev, args: argparse.Namespace, receipt: dict[str, Any]) -> None:
    frame = json.loads(dev.ssh("cat /run/overpressure/ui-frame.json"))
    if frame.get("install_preview") or not frame.get("live_log_event_driven"):
        message = "real event-driven UI was not started"
        raise RuntimeError(message)
    parts = dev.ssh("sha256sum /dev/block/by-name/recovery /usr/bin/pico4-recovery-ui", 60).splitlines()
    if [line.split()[0] for line in parts] != [receipt["recovery_sha256"], receipt["binary_sha256"]]:
        message = "unexpected recovery or UI readback hash"
        raise RuntimeError(message)
    for partition in ("boot", "super", "userdata"):
        print(partition, qualcomm.partition_size(args.backup, partition), flush=True)
    print("RECOVERY_PROBE_OK real_ui=1 stay_in_recovery=1", flush=True)


def _maintenance(dev: devloop.Dev, receipt: dict[str, Any], before: str) -> None:
    command = r"""
set -eu
for p in boot super misc recovery userdata cache persist; do
    [ "$(blockdev --getro /dev/block/by-name/"$p")" = 1 ]
done
flock -n /run/overpressure/install.lock true
pico4-install status | grep -qx 'bootmode: boot-recovery'
pico4-install status | grep -qx 'system: pico4-system'
pico4-install status | grep -qx 'data: pico4-data'
mountpoint=/run/overpressure/verify-data-ro
overpressure-mount-ro userdata "$mountpoint"
trap 'umount "$mountpoint"' EXIT
awk -v p="$mountpoint" '$2==p && $4 ~ /(^|,)ro(,|$)/ && $4 ~ /(^|,)(noload|norecovery)(,|$)/ {found=1}
END{exit !found}' /proc/mounts
test -s "$mountpoint/var/lib/pico4-stock/manifest.json"
umount "$mountpoint"
trap - EXIT
pico4-probe >/dev/null
test -s /run/overpressure/report/summary.txt
pico4-recovery-ui --input-selftest
"""
    if "INPUT_SELFTEST_OK" not in dev.ssh(command, 75) or guard(dev, receipt) != before:
        message = "incomplete Recovery maintenance checks"
        raise RuntimeError(message)
    print("RECOVERY_MAINTENANCE_OK readonly=1 data_mount_noload=1 inventory=1 input_filter=1", flush=True)


def _verify_task(task: dict[str, Any], lane: str) -> None:
    expected_ids = ["check", "boot", "super", "data", "keys", "assets", "finish"]
    if lane == "headset":
        expected_ids[1:1] = ["build-data", "build"]
    stages = task.get("stages", [])
    if (
        task.get("status") != "done"
        or task.get("locked")
        or not task.get("readonly_restored")
        or [s.get("id") for s in stages] != expected_ids
        or any(s.get("status") not in ("done", "skipped") for s in stages)
    ):
        message = "incomplete final task receipt"
        raise RuntimeError(message)
    for name in ("boot", "super"):
        stage = next(s for s in stages if s["id"] == name)
        done = {s["id"] for s in stage["substeps"] if s["status"] == "done"}
        if not {"write", "flush", "read", "digest"} <= done:
            message = f"missing actual {name} write, flush or readback receipt"
            raise RuntimeError(message)


def _install(dev: devloop.Dev, args: argparse.Namespace, receipt: dict[str, Any], before: str) -> None:
    start = time.monotonic()
    dev.install(
        args.system,
        stay_in_recovery=True,
        rewrite_images=True,
        on_headset=args.lane == "headset",
        boot=args.work / "valve-boot.img",
    )
    if guard(dev, receipt) != before:
        message = "Recovery restarted during install"
        raise RuntimeError(message)
    task = json.loads(dev.ssh("cat /run/overpressure/task.json"))
    _verify_task(task, args.lane)
    frame = json.loads(dev.ssh("cat /run/overpressure/ui-frame.json"))
    if frame.get("install_preview") or not frame.get("live_log_event_driven"):
        message = "unexpected UI identity"
        raise RuntimeError(message)
    (args.work.parent / f"task-{args.lane}.private.json").write_text(json.dumps(task, indent=2), encoding="utf-8")
    print(
        f"REAL_INSTALL_PASS lane={args.lane} duration_s={time.monotonic() - start:.2f} "
        "boot_recovery=1 readonly_restored=1 wrote_and_verified_boot_super=1",
        flush=True,
    )


def main() -> None:
    """Authenticate the dev session and run the selected Recovery acceptance lane."""
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("lane", choices=("probe", "pc", "headset", "maintenance"))
    for name in ("work", "settings", "backup", "identity", "system"):
        p.add_argument(f"--{name}", type=Path, required=True)
    a = p.parse_args()
    cfg = localconfig.load(a.settings, {})
    cfg.backup = str(a.backup)
    cfg.identity = str(a.identity)
    cfg.ssh = "ssh"
    cfg.platform_tools = str(Path(os.environ["USERPROFILE"]) / "Desktop/platform-tools")
    receipt = json.loads((a.work / "dev-image.json").read_text(encoding="utf-8"))
    devloop.RUN_ID = receipt["test_run"]
    dev = devloop.Dev(cfg, work=a.work, out=a.work.parent)
    before = guard(dev, receipt)
    dev.authenticate()
    if a.lane == "probe":
        _probe(dev, a, receipt)
    elif a.lane == "maintenance":
        _maintenance(dev, receipt, before)
    else:
        _install(dev, a, receipt, before)


if __name__ == "__main__":
    main()
