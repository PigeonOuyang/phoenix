# SPDX-License-Identifier: Unlicense
"""The PICO 4 in overpressure's registry and command line: registered, its steps, what touches it, its .env."""

from __future__ import annotations

import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from devices.phoenix.tooling import HEADSET, unlockfiles
from devices.phoenix.tooling import localconfig as pico4
from overpressure import __main__ as cli
from overpressure import disclaimer
from overpressure.headset import discover

ROOT = Path(__file__).resolve().parents[1]


class TouchesTests(unittest.TestCase):
    def test_what_touches_the_headset_and_what_does_not(self):
        touching = [
            ["dev", "install"],
            ["dev", "restore", "--installed"],
            ["dev", "ssh", "true"],
            ["device", "backup", "--identity", "x"],
            ["device", "unlock"],
            ["cycle", "--restore-only"],
            ["hwtest", "preflight"],
            ["install"],
            ["restore"],
            ["upgrade", "--json"],
            ["run", "install"],
            ["run", "restore-abl"],
        ]
        not_touching = [
            ["build", "--firmware", "x"],
            ["assets"],
            ["dev", "image"],
            ["dev", "boot-image"],
            ["device", "verify-backup", "--backup", "x"],
            ["device", "fetch-unlock-files"],
            ["install", "--plan"],
            ["dev", "install", "--help"],
            ["backup", "export", "x"],
            ["steps"],
            ["ram-boot"],  # the PICO 4 has none: it only says so
            ["run", "userspace"],
            ["run", "tools"],
        ]
        for argv in touching:
            self.assertTrue(cli.touches(HEADSET, argv), argv)
        for argv in not_touching:
            self.assertFalse(cli.touches(HEADSET, argv), argv)

    def test_the_command_line_asks_first_and_the_flag_accepts(self):
        with mock.patch.dict(os.environ, clear=False), mock.patch.object(cli.disclaimer, "gate", return_value=1):
            os.environ.pop(disclaimer.ENV, None)
            with mock.patch.object(HEADSET, "main") as ran:
                self.assertEqual(cli.main(["--device", "phoenix", "dev", "install"]), 1)
                ran.assert_not_called()
        with mock.patch.dict(os.environ, clear=False):
            os.environ.pop(disclaimer.ENV, None)
            with mock.patch.object(HEADSET, "main", return_value=0) as ran:
                self.assertEqual(cli.main(["--device", "phoenix", disclaimer.FLAG, "dev", "install"]), 0)
                ran.assert_called_once_with(["dev", "install"])
                self.assertTrue(disclaimer.accepted())


class RegistryTests(unittest.TestCase):
    def test_the_pico_4_is_registered(self):
        registry = discover()
        self.assertEqual(registry.get("phoenix").info.name, "PICO 4")
        with self.assertRaises(KeyError):
            registry.get("nonexistent")


class CommandLineTests(unittest.TestCase):
    def run_cli(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with (
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(out),
            mock.patch.dict(os.environ, {"OVERPRESSURE_DEVICE": ""}),
        ):
            code = cli.main(list(argv))
        return code, out.getvalue()

    def test_devices_and_steps(self):
        code, out = self.run_cli("devices")
        self.assertEqual(code, 0)
        self.assertIn("phoenix", out)
        code, out = self.run_cli("--device", "phoenix", "steps")
        self.assertEqual(code, 0)
        self.assertIn("-m overpressure --device phoenix build --firmware", out)

    def test_headset_commands_are_listed(self):
        code, out = self.run_cli()
        self.assertEqual(code, 2)
        self.assertIn("Commands of the PICO 4 (phoenix)", out)


class SettingsTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def test_the_pico4_env_example_keeps_the_known_engineering_abl_hash(self):
        env = self.root / ".env"
        env.write_text((ROOT / "env.example").read_text(encoding="utf-8"), encoding="utf-8")
        s = pico4.load(env, {})
        self.assertEqual(s.engineering_abl_sha256, unlockfiles.ENGINEERING_ABL_SHA256)
        s.require("engineering_abl_sha256")  # what dev stage-abl requires: there on a new computer


if __name__ == "__main__":
    unittest.main()
