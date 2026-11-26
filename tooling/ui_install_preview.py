#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
"""Prepare the isolated state stream for the native first-install UI demo."""

import argparse
import re
import shlex
import subprocess
from pathlib import Path

MESSAGES = (
    "Checking the packaged inputs, display and battery.",
    "Preparing the build data area.",
    "Building the SteamOS image on the headset. Total work is unknown.",
    "Writing boot and waiting for device readback.",
    "Writing super and waiting for device readback.",
    "Checking or preparing the user data area.",
    "Preparing the public keys and install ID.",
    "Verifying the system assets from the device backup.",
    "Setting normal boot and waiting for misc readback.",
)


def main() -> None:
    """Create isolated preview receipts with the actual task-state serializer."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new private output directory")
    parser.add_argument("--task-state", type=Path, required=True, help="actual Recovery task-state script")
    args = parser.parse_args()
    args.output.mkdir(mode=0o700)
    source = args.task_state.resolve()
    script = r"""
set -eu
umask 077
[ "${0##*/}" = sh ]
source=$1
out=$2
shift 2
PICO4_TASK_SOURCE=1
. "$source"
unset PICO4_TASK_SOURCE
# Redirect the same serializer before invoking any function. No broker,
# install commands, block devices or real /run/pico4 paths are used.
TASK_RUN=$out
TASK_DIR=$out/state
rm_receipts() {
    rm -f -- "$TASK_RUN/task.json" "$TASK_RUN/task.log"
    rm -r -- "$TASK_DIR"
}
printf '%s\n' 'Recovery preview. No storage operations are being performed.' \
    'Waiting for the host to start the simulated installation.' > "$out/boot.log"
task_init headset preview '系统安装'
i=0
for detail do
    if [ "$i" -gt 0 ]; then task_stage "$((i - 1))" done 'Simulated device step completed.'; fi
    task_stage "$i" running "$detail"
    cp "$TASK_RUN/task.json" "$out/task.$i.json"
    cp "$TASK_RUN/task.log" "$out/task.$i.log"
    i=$((i + 1))
done
task_stage "$((i - 1))" done 'Simulated normal boot selection confirmed.'
task_finish waiting 'Simulation complete. No reboot or first boot was attempted.'
task_readonly true
printf false > "$TASK_DIR/locked"
task_publish
printf '%s\n' 'Preview complete. SteamOS was not written or booted.' >> "$TASK_RUN/task.log"
cp "$TASK_RUN/task.json" "$out/task.$i.json"
cp "$TASK_RUN/task.log" "$out/task.$i.log"
rm_receipts
"""
    if not re.fullmatch(r"[\w./-]+", str(args.output.resolve())):
        parser.error("use a simple Linux directory path")
    subprocess.run(  # noqa: S603  # The Linux host's sh runs a fixed script; paths/messages are separate arguments.
        ["sh", "-c", script, "sh", str(source), str(args.output.resolve()), *MESSAGES],  # noqa: S607  # Linux host's sh.
        check=True,
    )
    print(f"Created {len(MESSAGES)} preview stages and a handoff receipt in {shlex.quote(str(args.output))}")


if __name__ == "__main__":
    main()
