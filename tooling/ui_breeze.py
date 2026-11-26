# SPDX-License-Identifier: Unlicense
"""Bake official SVGs into a role atlas, with no target SVG/runtime dependency.

Linux build host needs librsvg-2 and libcairo. Inputs are pinned originals in
the vendored manifest. Pixel format is premultiplied ARGB: R=foreground,
G=positive, B=negative, remaining alpha=surface knockout.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import re
import struct
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from _ctypes import CFuncPtr

SIDE = 88
COLS = 10
NS = "http://www.w3.org/2000/svg"
VISIBLE_ALPHA = 128


def signature(lib: ctypes.CDLL, name: str, out: type[object] | None, *types: type[object]) -> CFuncPtr:
    """Bind the argument and result types of a native library function."""
    f = getattr(lib, name)
    f.restype, f.argtypes = out, types
    return f


def main() -> None:  # noqa: C901, PLR0912, PLR0915  # Keep the pinned SVG transforms and checked atlas serialization in order.
    """Bake the authenticated Breeze SVG manifest into a native role atlas."""
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    p, i = ctypes.c_void_p, ctypes.c_int
    cairo = ctypes.CDLL("libcairo.so.2")
    rsvg = ctypes.CDLL("librsvg-2.so.2")
    gobject = ctypes.CDLL("libgobject-2.0.so.0")
    create_surface = signature(cairo, "cairo_image_surface_create", p, i, i, i)
    create = signature(cairo, "cairo_create", p, p)
    destroy = signature(cairo, "cairo_destroy", None, p)
    free_surface = signature(cairo, "cairo_surface_destroy", None, p)
    flush = signature(cairo, "cairo_surface_flush", None, p)
    data = signature(cairo, "cairo_image_surface_get_data", p, p)
    stride = signature(cairo, "cairo_image_surface_get_stride", i, p)
    parse = signature(rsvg, "rsvg_handle_new_from_data", p, ctypes.c_char_p, ctypes.c_size_t, p)
    unref = signature(gobject, "g_object_unref", None, p)

    class Rect(ctypes.Structure):
        """Describe the native librsvg viewport ABI."""

        _fields_ = [(name, ctypes.c_double) for name in ("x", "y", "width", "height")]

    draw = signature(rsvg, "rsvg_handle_render_document", i, p, p, ctypes.POINTER(Rect), p)
    ET.register_namespace("", NS)
    manifest = json.loads((args.source / "manifest.json").read_text(encoding="utf-8"))
    items = sorted(manifest["icons"], key=lambda item: item["key"])
    if len({item["key"] for item in items}) != len(items):
        raise AssertionError
    height = SIDE * ((len(items) + COLS - 1) // COLS)
    width = SIDE * COLS
    pixels = bytearray(width * height * 4)
    bounds = []
    for j, item in enumerate(items):
        raw = (args.source / "original" / item["resolved"]).read_bytes()
        if hashlib.sha256(raw).hexdigest() != item["sha256"]:
            msg = item["key"]
            raise AssertionError(msg)
        doc = ET.fromstring(raw)  # noqa: S314  # Only vendored SVGs matching the pinned SHA-256 are parsed.
        for node in list(doc.iter()):
            for child in list(node):
                if child.tag.rsplit("}", 1)[-1] in {"style", "metadata", "namedview"}:
                    node.remove(child)
            role = node.get("class", "")
            if role:
                node.attrib.pop("class")
                color = {
                    "ColorScheme-PositiveText": "#0f0",
                    "ColorScheme-NegativeText": "#00f",
                    "ColorScheme-Background": "#000",
                }.get(role, "#f00")
                node.set("color", color)
            if node.get("fill", "").lower() in {"#fff", "#ffffff"}:
                node.set("fill", "#000")
            for prop in ("fill", "stroke", "style"):
                s = node.get(prop)
                if s and re.search(r"url\(\s*[^#]", s):
                    msg = f"External paint in {item['key']}"
                    raise ValueError(msg)
        doc.set("color", "#f00")
        doc.set("fill", "currentColor")
        svg = ET.tostring(doc)
        h = parse(svg, len(svg), None)
        if not h:
            msg = item["key"]
            raise AssertionError(msg)
        surface = create_surface(0, SIDE, SIDE)
        context = create(surface)
        if not draw(h, context, ctypes.byref(Rect(0, 0, SIDE, SIDE)), None):
            msg = item["key"]
            raise AssertionError(msg)
        flush(surface)
        pitch = stride(surface)
        raw_pixels = ctypes.string_at(data(surface), pitch * SIDE)
        destroy(context)
        free_surface(surface)
        unref(h)
        visible = []
        for y in range(SIDE):
            for x in range(SIDE):
                b, g, r, a = raw_pixels[pitch * y + x * 4 : pitch * y + x * 4 + 4]
                if a >= VISIBLE_ALPHA:
                    visible.append((x, y))
                target = ((j // COLS * SIDE + y) * width + j % COLS * SIDE + x) * 4
                # PNG carries independent role weights, not a premultiplied color.
                pixels[target : target + 4] = bytes((r, g, b, max(0, a - r - g - b)))
        if not visible:
            msg = item["key"]
            raise AssertionError(msg)
        xs, ys = zip(*visible)
        bounds.append((min(xs), min(ys), max(xs) + 1, max(ys) + 1))
    args.output.mkdir(parents=True, exist_ok=True)

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    rows = b"".join(b"\0" + pixels[y * width * 4 : (y + 1) * width * 4] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    (args.output / "atlas.png").write_bytes(png)
    entries = "\n".join(f' {{"{item["key"]}",{",".join(map(str, bounds[j]))}}},' for j, item in enumerate(items))
    (args.output / "breeze_index.hpp").write_text(
        "// SPDX-License-Identifier: Unlicense\n"
        "// Generated atlas index by tooling/ui_breeze.py; icon artwork keeps its own license.\n#pragma once\n"
        f"#include <string_view>\nnamespace p4ui {{\n"
        f"inline constexpr int breeze_side={SIDE}, breeze_cols={COLS};\n"
        "struct BreezeEntry { std::string_view key; int l,t,r,b; };\n"
        f"inline constexpr BreezeEntry breeze_entries[]={{\n{entries}\n}};\n}}\n",
        encoding="utf-8",
    )
    print(f"{len(items)} Breeze mappings, atlas {width}×{height}, {len(png)} bytes")  # noqa: RUF001  # Dimension separator.


if __name__ == "__main__":
    main()
