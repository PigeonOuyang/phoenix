# SPDX-License-Identifier: Unlicense
"""Use the connected computer's configured UTC clock and current local offset."""

from __future__ import annotations

import datetime as dt
import json
import math
import time
from typing import Callable

MIN_EPOCH = 1577836800
MAX_EPOCH = 4102444800
MAX_OFFSET_SECONDS = 50400
CLOCK_READING_FIELDS = 2


def host_clock() -> tuple[int, int]:
    """Read the host's UTC epoch and configured local offset."""
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    delta = now.utcoffset()
    if delta is None:
        msg = "The host did not provide a usable time zone offset"
        raise RuntimeError(msg)
    epoch, offset = int(now.timestamp()), int(delta.total_seconds())
    if not MIN_EPOCH <= epoch < MAX_EPOCH or not -MAX_OFFSET_SECONDS <= offset <= MAX_OFFSET_SECONDS:
        msg = "The host clock or time zone offset is not initialized"
        raise RuntimeError(msg)
    return epoch, offset


def synchronize(ssh: Callable[..., str]) -> dict[str, object] | None:
    """Synchronize a supported Recovery clock and verify its running receipt."""
    # Old recovery images remain usable. This probe does not invoke date on an
    # installed system or alter its own time service.
    if ssh("test -x /usr/bin/pico4-time && echo available", 10, check=False).strip() != "available":
        return None
    epoch, offset = host_clock()
    started = time.monotonic()
    result = json.loads(ssh(f"pico4-time {epoch} {offset}", 10))
    elapsed = time.monotonic() - started
    if not isinstance(result, dict):
        msg = "Invalid Recovery time synchronization receipt"
        raise RuntimeError(msg)  # noqa: TRY004  # A malformed remote receipt is a runtime protocol failure.
    uptime = result.get("synchronized_uptime")
    if (
        result.get("source") != "host"
        or result.get("epoch_seconds") != epoch
        or result.get("offset_seconds") != offset
        or not result.get("boot_id")
        or isinstance(uptime, bool)
        or not isinstance(uptime, (float, int))
        or not math.isfinite(uptime)
        or uptime < 0
    ):
        msg = "Invalid Recovery time synchronization receipt"
        raise RuntimeError(msg)
    # Check the running clock, rather than treating a written marker as success.
    reading = ssh("date -u +%s; cat /proc/sys/kernel/random/boot_id", 10).split()
    if (
        len(reading) != CLOCK_READING_FIELDS
        or reading[1] != result["boot_id"]
        or not reading[0].isdigit()
        or not epoch <= int(reading[0]) <= epoch + math.ceil(elapsed + 10) + 1
    ):
        msg = "Could not verify the Recovery clock or boot ID after time synchronization"
        raise RuntimeError(msg)
    return result
