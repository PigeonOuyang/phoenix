# SPDX-License-Identifier: Unlicense
"""Run host regressions against this device checkout and an installed upstream.

Avoid silently testing a stale devices/phoenix copy in the upstream checkout.
Provide overpressure on PYTHONPATH; no device tools are invoked by these fakes.
"""

import importlib.util
import sys
import unittest
from pathlib import Path

import devices

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "devices.phoenix", ROOT / "__init__.py", submodule_search_locations=[str(ROOT)]
)
if spec is None or spec.loader is None:
    msg = f"Cannot load the phoenix checkout at {ROOT}"
    raise ImportError(msg)
module = importlib.util.module_from_spec(spec)
sys.modules["devices.phoenix"] = module
devices.__dict__["phoenix"] = module
spec.loader.exec_module(module)
suite = unittest.defaultTestLoader.loadTestsFromNames(
    [
        "devices.phoenix.tests.test_headset",
        "devices.phoenix.tests.test_device",
        "devices.phoenix.tests.test_recovery_time",
    ]
)
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
