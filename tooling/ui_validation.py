# SPDX-License-Identifier: Unlicense
"""Physical-device UI checks over authenticated SSH.

Key events are injected through the real evdev drivers, explicitly synthetic;
they do not establish physical button contact or optical panel visibility.
No installation, formatting, boot/super write or persist access is performed.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shlex
import struct
import subprocess
import time
from pathlib import Path
from typing import TypedDict

from ui_checkout import load  # ty: ignore[import-not-found]  # Direct script execution puts this directory on sys.path.

load()
from devices.phoenix.tooling import localconfig  # noqa: E402  # Import only after loading this checkout.
from devices.phoenix.tooling.devloop import Dev  # noqa: E402  # Import only after loading this checkout.

MAX_CLOCK_ERROR_SECONDS = 3


class UIState(TypedDict):
    """Describe fields read from the native renderer's frame receipt."""

    page: str
    language_durable: bool
    locale: str
    frame: int
    focus: int
    time_valid: bool
    timezone: str
    time_source: str
    date: str
    clock: str


class ValidationReport(TypedDict):
    """Keep measured checks separate from unverified physical observations."""

    physical_device: bool
    physical_button_contact: bool
    physical_panel_visibility: bool
    checks: list[dict[str, object]]


def main() -> None:  # noqa: C901, PLR0912, PLR0915  # Preserve the ordered hardware scenarios and cleanup.
    """Validate the native UI's real evdev, persistence, clock and log paths."""
    ap = argparse.ArgumentParser()
    for name in ("env", "work", "out", "backup", "identity", "results"):
        ap.add_argument("--" + name, type=Path, required=True)
    ap.add_argument("--restart-only", action="store_true")
    ap.add_argument("--capture-only", action="store_true")
    ap.add_argument("--time-only", action="store_true")
    ap.add_argument("--task-log-only", action="store_true")
    args = ap.parse_args()
    settings = localconfig.load(args.env, {})
    settings.backup, settings.identity = str(args.backup), str(args.identity)
    dev = Dev(settings, args.work, out=args.out)
    args.results.mkdir(parents=True, exist_ok=True)
    result: ValidationReport = {
        "physical_device": True,
        "physical_button_contact": False,
        "physical_panel_visibility": False,
        "checks": [],
    }

    def record(name: str, value: object) -> None:
        result["checks"].append({"name": name, "result": value})
        (args.results / "validation.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(name, json.dumps(value, ensure_ascii=False), flush=True)

    def state() -> UIState:
        return json.loads(dev.ssh("cat /run/overpressure/ui-frame.json"))

    def wait(page: str, *, durable: bool = True) -> UIState:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            value = state()
            if value["page"] == page and value["language_durable"] == durable:
                return value
            time.sleep(0.1)
        msg = f"page timeout {page}: {value}"
        raise RuntimeError(msg)

    def wait_persisted(previous_frame: int | None = None, *, require_time: bool = False) -> UIState:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            value = state()
            if (
                value["locale"] == "zh-CN"
                and value["language_durable"]
                and value["page"] in ("HOME", "CURRENT_TASK")
                and (not require_time or value.get("time_valid"))
                and (previous_frame is None or value["frame"] < previous_frame)
            ):
                return value
            time.sleep(0.1)
        msg = f"new process did not render its persisted locale: {value}"
        raise RuntimeError(msg)

    inventory = dev.ssh("pico4-recovery-ui --input-inventory")
    record("evdev_inventory", inventory)
    devices = {}
    for line in inventory.splitlines():
        for code, flag in ((116, "power=1"), (115, "volume_up=1"), (114, "volume_down=1")):
            if flag in line:
                devices[code] = line.split()[0]
    if set(devices) != {114, 115, 116}:
        msg = "three evdev keys not discovered"
        raise RuntimeError(msg)

    def key(code: int, count: int = 1) -> None:
        for _ in range(count):
            data = b"".join(
                struct.pack("<qqHHi", 0, 0, kind, keycode, value)
                for kind, keycode, value in ((1, code, 1), (0, 0, 0), (1, code, 0), (0, 0, 0))
            )
            subprocess.run(  # noqa: S603  # Authenticated SSH argv; the discovered evdev path is shell-quoted.
                dev.ssh_argv("cat > " + shlex.quote(devices[code])), input=data, check=True, timeout=30
            )
            time.sleep(0.08)

    def capture(name: str) -> Path:
        receipt = json.loads(dev.ssh("pico4-ui-shot", 60))
        remote = receipt["directory"]
        if not remote.startswith("/run/overpressure/screenshots/frame-"):
            msg = "invalid screenshot directory"
            raise RuntimeError(msg)
        folder = args.results / name
        folder.mkdir(exist_ok=True)
        for filename in ("双眼.png", "左眼.png", "右眼.png", "截图信息.json"):
            with (folder / filename).open("wb") as stream:
                subprocess.run(  # noqa: S603  # Authenticated SSH argv with a checked, quoted screenshot path.
                    dev.ssh_argv("cat " + shlex.quote(remote + "/" + filename)), stdout=stream, check=True, timeout=60
                )
        record(
            name,
            {
                "page": receipt["page"],
                "frame": receipt["frame"],
                "atomic_commit_completed": receipt["atomic_commit_completed"],
            },
        )
        return folder

    def home() -> None:
        value = wait("HOME")
        key(115, value["focus"])
        if state()["focus"] != 0:
            msg = "home focus did not return to its first row"
            raise RuntimeError(msg)

    def wait_time(zone: str | None = None, source: str | None = None) -> UIState:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            value = state()
            if (
                value.get("time_valid")
                and (zone is None or value["timezone"] == zone)
                and (source is None or value["time_source"] == source)
            ):
                return value
            time.sleep(0.1)
        msg = f"clock render timeout: {value}"
        raise RuntimeError(msg)

    if args.time_only:
        dev.sync_time()
        expected_zone = dt.datetime.now(dt.timezone.utc).astimezone().strftime("UTC%z")
        current = wait_time(expected_zone, "host")
        shown = dt.datetime.fromisoformat(current["date"] + "T" + current["clock"])
        host_local = dt.datetime.now(dt.timezone.utc).astimezone().replace(tzinfo=None, microsecond=0)
        if abs((shown - host_local).total_seconds()) > MAX_CLOCK_ERROR_SECONDS:
            msg = "UI clock differs from the configured host local clock"
            raise RuntimeError(msg)
        record("电脑本地时间与实机显示一致", current)
        for epoch, offset in (
            ("0", "28800"),
            ("4102444800", "28800"),
            ("1795543200", "50401"),
            ("1795543200", "-50401"),
            ("001795543200", "0"),
            ("not-a-time", "0"),
        ):
            command = shlex.join(["pico4-time", epoch, offset])
            reply = dev.ssh(f"if {command} >/dev/null 2>&1; then echo accepted; else echo rejected; fi")
            if reply.strip() != "rejected":
                msg = "invalid time input accepted"
                raise RuntimeError(msg)
        record("无效时间与时区输入拒绝", 6)
        if dev.ssh("test ! -e /etc/TZ && echo available", check=False).strip() == "available":
            try:
                dev.ssh("printf '%s\\n' UTC+05:30 > /etc/TZ")
                record("系统时区优先与无需重启更新", wait_time("UTC-0530"))
            finally:
                dev.ssh("rm -f /etc/TZ")
            record("恢复电脑时区偏移", wait_time(expected_zone))
        backup = "/run/overpressure/time-source.validation-backup"
        dev.ssh(f"test ! -e {backup} && mv /run/overpressure/time-source.json {backup}")
        try:
            record("无校时标记仍显示系统时钟", wait_time(expected_zone, "system"))
        finally:
            dev.ssh(f"mv {backup} /run/overpressure/time-source.json")
        record("恢复来源记录", wait_time(expected_zone, "host"))
        capture("时钟验证")
        record("complete", value=True)
        return
    readonly = (
        "for n in userdata cache; do d=$(readlink -f /dev/block/by-name/$n); "
        "p=$(basename $(readlink -f /sys/class/block/${d##*/}/..)); "
        "echo $n=$(blockdev --getro $d),parent=$(blockdev --getro /dev/$p); done; "
        "grep ui-preferences /proc/mounts || true"
    )

    def restart_checks() -> None:
        old_pid = dev.ssh("pidof pico4-recovery-ui").strip()
        previous_frame = state()["frame"]
        dev.ssh("kill -TERM " + old_pid)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            new_pid = dev.ssh("pidof pico4-recovery-ui", check=False).strip()
            if new_pid and new_pid != old_pid:
                break
            time.sleep(0.2)
        else:
            msg = "UI process did not respawn"
            raise RuntimeError(msg)
        record("进程重启语言保留", wait_persisted(previous_frame))
        capture("进程重启")
        old_boot = dev.ssh("cat /proc/sys/kernel/random/boot_id").strip()
        dev.ssh_reboot("pico4-reboot recovery")
        dev.reach(180)
        new_boot = dev.ssh("cat /proc/sys/kernel/random/boot_id").strip()
        if old_boot == new_boot:
            msg = "boot ID did not change"
            raise RuntimeError(msg)
        dev.sync_time()
        record("设备重启语言保留", wait_persisted(require_time=True))
        capture("设备重启")
        ro = dev.ssh(readonly)
        if "userdata=1,parent=1" not in ro or "cache=1,parent=1" not in ro or "ui-preferences" in ro:
            msg = "reboot partition/parent RO or mount cleanup failed"
            raise RuntimeError(msg)
        record("重启后块设备只读与卸载", ro)
        record("输入过滤自测", dev.ssh("pico4-recovery-ui --input-selftest"))
        record("内核异常检查", dev.ssh("dmesg | grep -E 'BUG:|Oops:|Kernel panic|smmu.*fault' || true"))
        record("complete", value=True)

    if args.restart_only:
        restart_checks()
        return
    if args.task_log_only:
        if state()["page"] == "LOG_VIEW":
            if state()["focus"] != 0:
                key(116)
            key(116)
        if state()["page"] == "CURRENT_TASK":
            key(115, state()["focus"])
            key(116)
        home()
        key(114, 7)
        key(116)
        wait("CURRENT_TASK")
        capture("当前任务")
        key(116)
        wait("LOG_VIEW")
        capture("任务日志")
        has_entries = dev.ssh("if [ -s /run/overpressure/task.log ]; then echo yes; else echo no; fi").strip() == "yes"
        key(114)
        if has_entries:
            if state()["focus"] != 1:
                msg = "volume did not select task log body"
                raise RuntimeError(msg)
            key(116)
            if state()["page"] != "LOG_VIEW" or state()["focus"] != 0:
                msg = "short power must first select Back"
                raise RuntimeError(msg)
        elif state()["page"] != "LOG_VIEW" or state()["focus"] != 0:
            msg = "empty task log must retain Back selection"
            raise RuntimeError(msg)
        key(116)
        wait("CURRENT_TASK")
        key(115)
        key(116)
        home()
        record("complete", value=True)
        return
    if args.capture_only:
        if state()["page"] == "CURRENT_TASK":
            key(115, state()["focus"])
            key(116)
        home()
        capture("首页")
        key(114, 7)
        key(116)
        wait("CURRENT_TASK")
        capture("当前任务")
        key(115)
        key(116)
        home()
        key(114, 5)
        key(116)
        wait("DISPLAY")
        capture("分层视差")
        key(116)
        wait("DISPLAY")
        pair = capture("无视差")
        if (pair / "左眼.png").read_bytes() != (pair / "右眼.png").read_bytes():
            msg = "zero disparity images differ"
            raise RuntimeError(msg)
        record("无视差双眼逐字节相同", value=True)
        key(115)
        key(116)
        home()
        key(114, 4)
        key(116)
        wait("DIAG")
        key(114, 2)
        key(116)
        wait("LOGS")
        key(116)
        wait("LOG_VIEW")
        capture("全屏日志")
        key(116)
        wait("LOGS")
        key(115)
        key(116)
        wait("DIAG")
        key(115, 3)
        key(116)
        home()
        record("complete", value=True)
        return
    initial = state()
    if initial["page"] == "CURRENT_TASK" and initial["language_durable"]:
        key(115, initial["focus"])
        key(116)
        initial = wait("HOME")
    if initial["page"] == "LANGUAGE":
        capture("首次进入")
        key(116)
        record("首次语言保存与全屏日志", wait("LOG_VIEW"))
        capture("首次全屏日志")
        key(116)
        record("全屏默认返回", wait("CURRENT_TASK"))
        capture("当前任务")
        key(115)
        key(116)
        wait("HOME")
    elif initial["page"] == "HOME" and initial["language_durable"]:
        capture("启动语言保留")
        key(116)  # Status
        wait("STATUS")
        key(114, 3)  # Language
        key(116)
        wait("LANGUAGE")
        key(116)
        record("已有语言重新保存", wait("STATUS"))
        key(115, 4)
        key(116)
        wait("HOME")
    else:
        msg = f"suite requires LANGUAGE or persisted HOME: {initial}"
        raise RuntimeError(msg)
    # Open the real boot log through ordinary navigation. No fixture task/log
    # files are injected, and identity status must not manufacture a task.
    home()
    key(114, 4)
    key(116)
    wait("DIAG")
    key(114, 2)
    key(116)
    wait("LOGS")
    key(116)
    wait("LOG_VIEW")
    capture("启动日志")
    key(114)
    if state()["focus"] != 1:
        msg = "volume did not select boot log body"
        raise RuntimeError(msg)
    key(116)
    if state()["page"] != "LOG_VIEW" or state()["focus"] != 0:
        msg = "short power must first select Back"
        raise RuntimeError(msg)
    key(116)
    record("全屏默认返回", wait("LOGS"))
    key(115)
    key(116)
    wait("DIAG")
    key(115, 3)
    key(116)
    home()
    key(114, 7)
    key(116)
    wait("CURRENT_TASK")
    capture("当前任务")
    key(116)  # selected overview
    wait("LOG_VIEW")
    key(116)
    wait("CURRENT_TASK")
    key(115)
    key(116)
    home()
    key(114, 5)
    key(116)
    wait("DISPLAY")
    capture("分层视差")
    key(116)
    wait("DISPLAY")
    pair = capture("无视差")
    if (pair / "左眼.png").read_bytes() != (pair / "右眼.png").read_bytes():
        msg = "zero disparity images differ"
        raise RuntimeError(msg)
    record("无视差双眼逐字节相同", value=True)
    key(115)  # Back; cancels the session draft
    key(116)
    home()
    ro = dev.ssh(readonly)
    if "userdata=1,parent=1" not in ro or "cache=1,parent=1" not in ro or "ui-preferences" in ro:
        msg = "partition/parent RO or mount cleanup failed"
        raise RuntimeError(msg)
    record("语言保存后块设备只读与卸载", ro)
    unsupported = dev.ssh("pico4-install preferences-set en-US; echo RESULT=$?", check=False)
    if "RESULT=2" not in unsupported:
        msg = "unsupported language was accepted"
        raise RuntimeError(msg)
    record("未实现语言拒绝", value=True)
    conflict = dev.ssh(
        "flock /run/overpressure/install.lock sh -c 'pico4-install preferences-set zh-CN; echo RESULT=$?'", check=False
    )
    if "RESULT=1" not in conflict or '"durable":false' not in conflict:
        msg = "concurrent language store did not refuse"
        raise RuntimeError(msg)
    record("安装锁冲突拒绝", value=True)
    restart_checks()


if __name__ == "__main__":
    main()
