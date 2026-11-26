# SPDX-License-Identifier: Unlicense
"""The real BusyBox task broker, with a temporary directory and a no-device installer stub."""

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from queue import Queue

ROOT = Path(__file__).resolve().parents[1]
SHELL = shlex.split(os.environ.get("PICO4_TEST_ASH", "busybox ash"))
FLOCK = shutil.which("flock")
TOKEN = "b" * 32
PAYLOAD = bytes(range(256)) * 1024


def request(argv: list[str], op: str, *args: str, payload: bytes = b"") -> subprocess.CompletedProcess[bytes]:
    """Send one request to the temporary broker, keeping its byte stream intact."""
    return subprocess.run(  # noqa: S603
        [*argv, op, TOKEN, *args], input=payload, capture_output=True, timeout=8, check=False
    )


def request_with_open_stdin(argv: list[str], op: str, *args: str) -> tuple[int, bytes]:
    """Exercise an SSH session whose client has not closed its input pipe."""
    with subprocess.Popen(  # noqa: S603
        [*argv, op, TOKEN, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    ) as process:
        try:
            rc = process.wait(timeout=8)
            if process.stdout is None:
                msg = "request output pipe unavailable"
                raise RuntimeError(msg)
            return rc, process.stdout.read()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)


def state(root: Path) -> dict:
    """Read the broker's last published task state."""
    return json.loads((root / "task.json").read_text(encoding="utf-8"))


def lock_result(root: Path, flock: str) -> int:
    """Try a second nonblocking lock on the broker's temporary lock file."""
    return subprocess.run(  # noqa: S603
        [flock, "-n", str(root / "install.lock"), "true"], check=False, timeout=3
    ).returncode


@unittest.skipUnless(os.name == "posix" and SHELL and shutil.which(SHELL[0]) and FLOCK, "needs BusyBox ash and flock")
class RecoveryTaskTransportTests(unittest.TestCase):
    """Exercise the installer transport without access to any partition."""

    def test_lock_and_binary_input_survive_backgrounding(self) -> None:  # noqa: PLR0915  # One broker lifetime.
        """Verify that ash preserves both long-lived SSH input and request streams."""
        if FLOCK is None:
            self.skipTest("flock is unavailable")
        for kind, command, stages in (("host", "write", 7), ("headset", "build", 9), ("restore", "write", 7)):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                stub = root / "installer"
                stub.write_text(
                    'case "$1" in\n'
                    'write | build) if [ "${2:-}" = --from-data ]; then test ! -t 0 && echo local; '
                    'else sha256sum | cut -d " " -f 1; fi ;;\n'
                    "build-file) echo '{\"ok\":true}' ;;\n"
                    "hash) echo refused >&2; exit 7 ;;\n"
                    "stock) echo refused-before-input >&2; exit 9 ;;\n"
                    "esac\n",
                    encoding="utf-8",
                )
                source = (ROOT / "initramfs/usr/libexec/pico4/task-state").read_text(encoding="utf-8")
                helper = root / "task-state"
                helper.write_text(
                    source.replace("TASK_RUN=/run/overpressure", f"TASK_RUN={shlex.quote(temporary)}").replace(
                        "sh /usr/bin/pico4-install", f"sh {shlex.quote(str(stub))}"
                    ),
                    encoding="utf-8",
                )
                argv = [*SHELL, str(helper)]

                with subprocess.Popen(  # noqa: S603
                    [*argv, "hold", TOKEN, kind], stdin=subprocess.PIPE, stdout=subprocess.PIPE
                ) as holder:
                    inbox: Queue[bytes] = Queue()
                    if holder.stdin is None or holder.stdout is None:
                        self.fail("broker pipes unavailable")
                    reader = threading.Thread(
                        target=lambda out=holder.stdout, queue=inbox: queue.put(out.readline()), daemon=True
                    )
                    reader.start()
                    try:
                        self.assertEqual(inbox.get(timeout=5), f"locked {TOKEN}\n".encode())
                        initial = state(root)
                        self.assertEqual(len(initial["stages"]), stages)
                        self.assertEqual(initial["detail"], "Checking inputs and device")
                        log = root / "task.log"
                        opening = b"[running] Checking inputs and device\n"
                        if kind != "restore":
                            opening += b"[running] Check inputs and device\n"
                        self.assertEqual(log.read_bytes(), opening)
                        self.assertEqual(request_with_open_stdin(argv, "stage", "0", "done", "checked"), (0, b""))
                        self.assertEqual(log.read_bytes(), opening + b"[done] checked\n")
                        self.assertEqual(lock_result(root, FLOCK), 1)
                        stream = request(argv, "run", command, payload=PAYLOAD)
                        self.assertEqual(stream.returncode, 0, stream.stderr)
                        self.assertEqual(stream.stdout.strip().decode(), hashlib.sha256(PAYLOAD).hexdigest())
                        record = request(argv, "run", "build-file")
                        self.assertEqual(record.returncode, 0, record.stderr)
                        self.assertEqual(json.loads(record.stdout), {"ok": True})
                        self.assertTrue(log.read_bytes().endswith(record.stdout))
                        refused = request(argv, "run", "hash")
                        self.assertEqual(refused.returncode, 7)
                        self.assertEqual(refused.stdout, b"refused\n")
                        self.assertTrue(log.read_bytes().endswith(refused.stdout))
                        self.assertEqual(request_with_open_stdin(argv, "run", "write", "--from-data"), (0, b"local\n"))
                        self.assertEqual(request_with_open_stdin(argv, "run", "stock"), (9, b"refused-before-input\n"))
                        if kind == "host":
                            self.check_interrupted_progress(argv, root)
                        elif kind == "restore":
                            # An older host's detail still passes through without translation.
                            legacy = "\u65e7\u56de\u6267"
                            self.assertEqual(request_with_open_stdin(argv, "stage", "0", "done", legacy), (0, b""))
                            self.assertEqual(state(root)["detail"], legacy)
                            self.assertTrue(log.read_bytes().endswith(f"[done] {legacy}\n".encode()))
                            self.assertEqual(request_with_open_stdin(argv, "finish", "done", "verified"), (0, b""))
                        self.assertIsNone(holder.poll())
                    finally:
                        holder.stdin.close()
                        try:
                            holder.wait(timeout=6)
                        except subprocess.TimeoutExpired:
                            holder.kill()
                            holder.wait(timeout=3)
                    self.assertEqual(holder.returncode, 0)
                final = state(root)
                self.assertEqual(final["status"], "done" if kind == "restore" else "error")
                if kind == "headset":
                    disconnected = "Host workflow disconnected; see the original log"
                    self.assertEqual(final["error"], disconnected)
                    self.assertTrue(log.read_bytes().endswith(f"[error] {disconnected}\n".encode()))
                self.assertFalse(final["locked"])
                self.assertEqual(lock_result(root, FLOCK), 0)

    def check_interrupted_progress(self, argv: list[str], root: Path) -> None:
        self.assertEqual(request(argv, "stage", "2", "running", "system image").returncode, 0)
        self.assertEqual(request(argv, "phase", "inspect", "done").returncode, 0)
        self.assertEqual(request(argv, "phase", "write", "running").returncode, 0)
        accepted = request(argv, "phase", "write", "running", "40", "100", "bytes", "local-written-bytes")
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        plan = {s["id"]: s for s in state(root)["stages"][2]["substeps"]}
        self.assertGreater(plan["write"]["weight"], plan["read"]["weight"])
        self.assertEqual((plan["write"]["done"], plan["write"]["total"]), (40, 100))
        self.assertTrue(plan["read"]["measured"])
        for count, source_name in (("39", "local-written-bytes"), ("90", "host-input-bytes")):
            rejected = request(argv, "phase", "write", "running", count, "100", "bytes", source_name)
            self.assertNotEqual(rejected.returncode, 0)
        self.assertEqual(request(argv, "finish", "error", "interrupted").returncode, 0)
        plan = {s["id"]: s for s in state(root)["stages"][2]["substeps"]}
        self.assertEqual((plan["write"]["status"], plan["write"]["done"]), ("error", 40))
        self.assertEqual(plan["read"]["status"], "pending")
