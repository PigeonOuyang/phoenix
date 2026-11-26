#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Exercise the live reader and capture physical KMS receipts using synthetic evdev input."""

import argparse
import base64
import re
import statistics
import struct
import subprocess
from pathlib import Path

SAMPLE_FIELDS = 7
MIN_COAST_POSITIONS = 2
READER_FIELDS = (
    r'"frame"|"draw_ms"|"flip_monotonic_s"|'
    r'"reader_coasting"|"reader_catching_up"|"reader_visual_top_px"|"reader_velocity_rows_s"'
)


def key(code: int, value: int) -> str:
    """Encode an evdev key event and its synchronization event for the remote probe."""
    return base64.b64encode(struct.pack("<qqHHi", 0, 0, 1, code, value) + struct.pack("<qqHHi", 0, 0, 0, 0, 0)).decode()


def main() -> None:
    """Probe physical input pacing and require displacement during release coasting."""
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("work", type=Path, help="physical dev folder with the disposable key")
    ap.add_argument("--direction", choices=("up", "down"), default="up")
    ap.add_argument("--name", default="reader-pacing.private.txt")
    ap.add_argument("--open-boot-log", action="store_true", help="navigate to the kernel log after boot")
    args = ap.parse_args()
    options = [
        "ssh",
        "-T",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=4",
        "-o",
        f"UserKnownHostsFile={args.work / 'known_hosts'}",
        "-i",
        str(args.work / "id_ed25519"),
        "root@172.16.42.1",
    ]
    inv = subprocess.run(  # noqa: S603  # SSH uses the explicit dev key and fixed host and inventory command, no shell.
        [*options, "pico4-recovery-ui --input-inventory"], capture_output=True, text=True, timeout=12, check=True
    ).stdout
    nodes = {}
    volume = "volume_" + args.direction
    code = 115 if args.direction == "up" else 114
    for label in ("volume_up", "volume_down", "power"):
        found = re.findall(rf"^(/dev/input/event[0-9]+) .* {label}=1(?=[ \r\n]|$)", inv, re.MULTILINE)
        if len(found) != 1:
            message = f"expected one physical {label} key"
            raise RuntimeError(message)
        nodes[label] = found[0]
    navigation = ""
    if args.open_boot_log:
        navigation = f"""
tap() {{
    event "$1" "$3"
    usleep 85000
    event "$2" "$3"
    usleep 330000
    grep -E '"page"|"focus"' "$f" >&2
}}
page() {{ grep -q '"page":"'"$1"'"' "$f"; }}
focus() {{ grep -q '"focus":'"$1"',' "$f"; }}
down() {{ tap {key(114, 1)} {key(114, 0)} {nodes["volume_down"]}; }}
up() {{ tap {key(115, 1)} {key(115, 0)} {nodes["volume_up"]}; }}
power() {{ tap {key(116, 1)} {key(116, 0)} {nodes["power"]}; }}
if page HOME; then
    for i in 1 2 3 4 5 6 7 8 9; do if ! focus 4; then page HOME; down; fi; done
    focus 4
    page HOME
    power
fi
if page REPORT; then
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
        if ! focus 0; then page REPORT; down; fi
    done
fi
if page REPORT && focus 0; then power; fi
if page DIAG; then
    for i in 1 2 3; do if ! focus 3; then page DIAG; down; fi; done
    focus 3
    page DIAG
    power
fi
if page LOGS; then
    if focus 0; then down; fi
    focus 1
    down
    focus 2
    page LOGS
    power
fi
page LOG_VIEW
"""
    cmd = f"""
set -eu
f=/run/overpressure/ui-frame.json
grep -q '"install_preview":false' "$f"
grep -qw overpressure.part=recovery /proc/cmdline
event() {{ printf '%s' "$1" | base64 -d > "$2"; }}
{navigation}
grep -q '"page":"LOG_VIEW"' "$f"
if ! grep -q '"log_selected_line":[0-9]' "$f"; then
    event {key(code, 1)} {nodes[volume]}
    usleep 80000
    event {key(code, 0)} {nodes[volume]}
    usleep 120000
fi
sample() {{
  i=0
  while [ "$i" -lt "$1" ]; do
    awk -F: '/{READER_FIELDS}/ {{
      gsub(/[ ,]/, "", $2); printf "%s ", $2
    }} END {{print ""}}' "$f"
    usleep 22000
    i=$((i+1))
  done
}}
event {key(code, 1)} {nodes[volume]}
sample 58
event {key(code, 0)} {nodes[volume]}
sample 4
event {key(code, 1)} {nodes[volume]}
sample 25
event {key(code, 0)} {nodes[volume]}
sample 15
if grep -q '"install_live_log":true' "$f" && grep -q '"follow_install_log":false' "$f"; then
    event {key(116, 1)} {nodes["power"]}
    usleep 100000
    event {key(116, 0)} {nodes["power"]}
fi
"""
    child = subprocess.run(  # noqa: S603  # SSH uses fixed probe text and regex-validated evdev nodes on the dev host.
        [*options, cmd],
        capture_output=True,
        text=True,
        timeout=24,
        check=False,
    )
    if child.returncode:
        message = f"device reader probe stopped ({child.returncode}): {child.stderr.strip()}"
        raise RuntimeError(message)
    data = child.stdout
    (args.work.parent / args.name).write_text(data, encoding="utf-8")
    samples = [line.split() for line in data.splitlines() if len(line.split()) == SAMPLE_FIELDS]
    # Fields appear in the order in which the native UI publishes them.
    frames = {int(s[0]): s for s in samples}
    coasts = [s for s in frames.values() if s[3] == "true"]
    catches = [s for s in frames.values() if s[4] == "true"]
    print(
        f"unique_frames={len(frames)} coast_frames={len(coasts)} catch_frames={len(catches)} "
        f"median_draw_ms={statistics.median(float(s[1]) for s in frames.values()):.2f}"
    )
    if not coasts or len({s[2] for s in coasts}) < MIN_COAST_POSITIONS:
        message = "did not observe viewport displacement during the real release coast"
        raise RuntimeError(message)


if __name__ == "__main__":
    main()
