# SPDX-License-Identifier: Unlicense
"""Validate real NCM rate updates with bounded transfers to/from volatile memory."""

import argparse
import json
import subprocess
import time
from pathlib import Path

from ui_checkout import load  # ty: ignore[import-not-found]  # Direct script execution puts this directory on sys.path.

load()
from devices.phoenix.tooling import localconfig  # noqa: E402  # Import only after loading this checkout.
from devices.phoenix.tooling.devloop import Dev  # noqa: E402  # Import only after loading this checkout.

MIN_DISTINCT_UPDATES = 15
MIN_REFRESH_SECONDS = 0.9
MAX_REFRESH_SECONDS = 1.25
MIN_DIRECTION_SAMPLES = 3
MIN_COUNTER_DELTA_CHECKS = 20


def main() -> None:  # noqa: C901, PLR0912, PLR0915  # Keep bounded transfers, cleanup and measured checks in order.
    """Measure NCM refresh timing and bidirectional rates without storage writes."""
    ap = argparse.ArgumentParser()
    for name in ("env", "work", "out", "backup", "identity", "results"):
        ap.add_argument("--" + name, type=Path, required=True)
    args = ap.parse_args()
    settings = localconfig.load(args.env, {})
    settings.backup, settings.identity = str(args.backup), str(args.identity)
    dev = Dev(settings, args.work, out=args.out)
    args.results.mkdir(parents=True, exist_ok=True)
    initial = json.loads(dev.ssh("cat /run/overpressure/ui-frame.json"))
    if initial.get("ncm_state") != "connected" or initial.get("ncm_interface") != "usb0":
        msg = "expected the actual configured usb0 NCM link"
        raise RuntimeError(msg)
    # One persistent observer avoids adding an SSH handshake to every sample.
    command = 'i=0; while [ "$i" -lt 125 ]; do cat /run/overpressure/ui-frame.json; i=$((i+1)); sleep 0.2; done'
    with (
        (args.results / "samples.jsonl").open("wb") as samples,
        (args.results / "observer.stderr").open("wb") as errors,
    ):
        observer = subprocess.Popen(  # noqa: S603  # Authenticated SSH argv with a fixed bounded observer command.
            dev.ssh_argv(command), stdout=samples, stderr=errors
        )
        try:
            time.sleep(2)
            block = bytes(256 * 1024)
            start_download = time.monotonic()
            # Computer -> headset is the displayed DOWN arrow.
            push = subprocess.Popen(  # noqa: S603  # Authenticated SSH argv; transfer data goes only to /dev/null.
                dev.ssh_argv("cat > /dev/null"),
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            try:
                if push.stdin is None or push.stderr is None:
                    msg = "NCM transfer pipes were not created"
                    raise RuntimeError(msg)
                for _ in range(60):
                    push.stdin.write(block)
                    push.stdin.flush()
                    time.sleep(0.1)
                push.stdin.close()
                if push.wait(timeout=15):
                    msg = push.stderr.read().decode(errors="replace")
                    raise RuntimeError(msg)
            finally:
                if push.poll() is None:
                    push.kill()
                    push.wait()
                if push.stderr is not None:
                    push.stderr.close()
            download_duration = time.monotonic() - start_download
            time.sleep(2)
            start_upload = time.monotonic()
            # Headset -> computer is the displayed UP arrow. Only /dev/zero is read.
            subprocess.run(  # noqa: S603  # Authenticated SSH argv; a fixed bounded command reads only /dev/zero.
                dev.ssh_argv(
                    'i=0; while [ "$i" -lt 60 ]; do '
                    "dd if=/dev/zero bs=262144 count=1 2>/dev/null; "
                    "i=$((i+1)); sleep 0.1; done"
                ),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                check=True,
                timeout=20,
            )
            upload_duration = time.monotonic() - start_upload
            observer.wait(timeout=35)
            if observer.returncode:
                msg = "NCM observer failed"
                raise RuntimeError(msg)
        finally:
            if observer.poll() is None:
                observer.kill()
                observer.wait()
    raw = (args.results / "samples.jsonl").read_text(encoding="utf-8")
    decoder, records = json.JSONDecoder(), []
    while raw.strip():
        raw = raw.lstrip()
        item, end = decoder.raw_decode(raw)
        records.append(item)
        raw = raw[end:]
    unique = []
    for item in records:
        stamp = item.get("network_sample_seconds")
        if stamp is not None and (not unique or stamp != unique[-1]["network_sample_seconds"]):
            unique.append(item)
    if len(unique) < MIN_DISTINCT_UPDATES:
        msg = "insufficient distinct network refreshes"
        raise RuntimeError(msg)
    intervals, exact = [], 0
    for previous, current in zip(unique, unique[1:]):
        elapsed = current["network_sample_seconds"] - previous["network_sample_seconds"]
        if not MIN_REFRESH_SECONDS <= elapsed <= MAX_REFRESH_SECONDS:
            msg = f"network refresh interval {elapsed}"
            raise RuntimeError(msg)
        intervals.append(elapsed)
        for counter, rate in (("network_rx_bytes", "download_bps"), ("network_tx_bytes", "upload_bps")):
            before, after, actual = previous.get(counter), current.get(counter), current.get(rate)
            if before is None or after is None or actual is None:
                continue
            expected = (after - before) / elapsed
            if expected < 0 or abs(actual - expected) > max(0.001, expected * 1e-9):
                msg = f"rate does not match observed counter delta: {rate}"
                raise RuntimeError(msg)
            exact += 1
    down = [
        item
        for item in unique
        if (item.get("download_bps") or 0) > 1024 * 1024
        and (item.get("download_bps") or 0) > 4 * (item.get("upload_bps") or 0)
    ]
    up = [
        item
        for item in unique
        if (item.get("upload_bps") or 0) > 1024 * 1024
        and (item.get("upload_bps") or 0) > 4 * (item.get("download_bps") or 0)
    ]
    if len(down) < MIN_DIRECTION_SAMPLES or len(up) < MIN_DIRECTION_SAMPLES or exact < MIN_COUNTER_DELTA_CHECKS:
        msg = "sustained bidirectional rate evidence incomplete"
        raise RuntimeError(msg)
    report = {
        "passed": True,
        "physical_device": True,
        "interface": "usb0",
        "ncm_state": initial["ncm_state"],
        "counter_delta_checks": exact,
        "distinct_updates": len(unique),
        "refresh_min_seconds": min(intervals),
        "refresh_max_seconds": max(intervals),
        "computer_to_headset_bytes": len(block) * 60,
        "headset_to_computer_bytes": len(block) * 60,
        "download_transfer_seconds": download_duration,
        "upload_transfer_seconds": upload_duration,
        "download_samples_above_1MiBps": len(down),
        "upload_samples_above_1MiBps": len(up),
        "peak_download_bps": max(item["download_bps"] for item in down),
        "peak_upload_bps": max(item["upload_bps"] for item in up),
        "counter_direction": {"upload": "headset tx", "download": "headset rx"},
        "transfer_storage": "/dev/null and /dev/zero",
        "internet_connectivity_tested": False,
    }
    (args.results / "network-validation.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
