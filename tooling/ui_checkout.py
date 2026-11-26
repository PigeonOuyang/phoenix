# SPDX-License-Identifier: Unlicense
"""Load UI development tools from this checkout, not a host dependency's copy."""

import importlib.util
import sys
from pathlib import Path

import devices


def load() -> None:
    """Bind this checkout as the phoenix package before importing its tooling."""
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location(
        "devices.phoenix", root / "__init__.py", submodule_search_locations=[str(root)]
    )
    if spec is None or spec.loader is None:
        msg = f"Cannot load the phoenix checkout at {root}"
        raise ImportError(msg)
    module = importlib.util.module_from_spec(spec)
    sys.modules["devices.phoenix"] = module
    devices.__dict__["phoenix"] = module
    spec.loader.exec_module(module)
