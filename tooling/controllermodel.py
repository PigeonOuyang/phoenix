# SPDX-License-Identifier: Unlicense
"""SteamVR render models of the PICO 4 controllers, from the models PICO OS ships.

PICO OS keeps a textured model of each PICO 4 controller for its SDK's controller loader in
`/system/pre_resource/data/misc/user/controller/Neo3Phx/` (copied to /data/misc/user/controller
on first boot): `60.obj` (left) and `61.obj` (right), Maya exports in centimetres with
triangles and quads, positions, UVs and normals, each in a group `Controller` (the body) and a
group `Power` (the six triangles of the battery indicator, drawn with another texture). Their
textures `60_*.png` and `61_*.png` (1024 x 1024 RGBA) are twelve identical files per hand, one
per button state; `*_idle.png` is used. The models' axes and origin are those of the aim pose
PICO OS publishes (x right, y up, z back): PICO OS's own nominal LED rings
(calibration/controllers/led-models.json), taken into the aim frame as the tracker does
(aim = (-m.y, m.x, m.z), less the lever arm of lever-arm.json), lie on the models' surface
without any rotation or offset. `--check` verifies that for every LED.

For each hand this writes a SteamVR render model directory `pico4_controller_<hand>/`: the model
as triangulated OBJs in metres with its original MTL and PNG, and a JSON with the body component and the
pose components `aim`, `openxr_aim` and `tip` (the model's origin), plus `grip`, `openxr_grip`
and `handgrip` (the name in SteamVR's pico_controller profile) from PICO OS's aim-to-grip
transform when it is given. SteamVR's system UI draws its controller status panel, with the battery's
pips, on a render model's `status` component, so PICO OS's indicator becomes that component, its UVs
on the pips of the panel: the charge shows where PICO OS shows it. For the known disconnected
mesh topology, buttons, trigger, grip and thumbstick become separate components with SteamVR's
native `motion` descriptions. Unknown topology keeps the static model. The models are vendor data:
they are read from the headset's own firmware and stay out of the repository.

    python -m devices.phoenix.tooling.controllermodel MODEL_DIR OUTPUT_DIR
        [--aim-to-grip JSON] [--check LED_JSON LEVER_JSON]
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Sequence

HANDS = {"left": "60", "right": "61"}
NAME = "pico4_controller_{hand}"
#: The group that holds the body; the battery indicator ("Power") uses a texture of its own.
BODY_GROUP = "Controller"
POWER_GROUP = "Power"
#: SteamVR's status panel is this many pixels square; its battery pips fill this part of it (left, top,
#: right, bottom), as its stylesheet lays them out.
STATUS_PANEL_PX = 256
STATUS_BATTERY_PX = (23, 172, 233, 197)
CM = 0.01
#: Plausibility: the body's extent along each axis (metres).
MIN_EXTENT_M, MAX_EXTENT_M = 0.04, 0.25
#: The left model is the right one mirrored in x within this (metres), by bounding box.
MIRROR_TOLERANCE_M = 0.002
#: Every LED of the nominal ring lies within this distance of the body's surface (metres).
LED_SURFACE_TOLERANCE_M = 0.003
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
#: A face has at least three corners, each with three indices (v/vt/vn).
MIN_CORNERS = CORNER_INDICES = 3
#: The aim-to-grip quaternion's y and z within this of zero: a turn about x alone.
AXIS_TOLERANCE = 1e-9
#: Triangle counts of the stock model's disconnected parts, largest first. No vertex or UV data.
PART_TRIANGLES = (2234, 650, 606, 224, 215, 160, 160, 160, 96, 14)
#: Weld only for connectivity (0.1 micrometre); retain the original positions, UVs and normals.
WELD_DECIMALS = 7
#: Nominal visual travel, not controller calibration.
BUTTON_TRAVEL_M = 0.001
TRIGGER_TRAVEL_DEG = -15.0
GRIP_TRAVEL_DEG = 11.0
STICK_TRAVEL_DEG = 20.0
#: Two circular cross-sections determine the bearing centre.
BEARING_RINGS = 2
#: A separate render-model bit for the extra face button. The driver still publishes its
#: independent input path; SteamVR requires a component bit even for modern path-based motion.
EXTRA_MODEL_BUTTON = 8


class ControllerModelError(ValueError):
    """The models are not the PICO 4 controllers' as this tool knows them."""


@dataclass
class Mesh:
    """The body of one model: positions (metres), UVs, normals and triangles of index triples."""

    positions: list[tuple[float, float, float]] = field(default_factory=list)
    uvs: list[tuple[float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    #: Each corner is (position, uv, normal), 0-based into the lists above.
    triangles: list[tuple[tuple[int, int, int], ...]] = field(default_factory=list)


def parse_obj(text: str, hand: str, wanted: str = BODY_GROUP) -> Mesh:
    """Return one group of a PICO OS controller OBJ, in metres, triangulated and compactly indexed."""
    if "centimeters" not in text.split("\n", 1)[0]:
        msg = f"{hand}: not a model in centimetres"
        raise ControllerModelError(msg)
    sources, faces = _read_obj(text, hand, wanted)
    if not faces:
        msg = f"{hand}: no face in a group {wanted!r}"
        raise ControllerModelError(msg)
    return _compact(sources, faces, hand)


def _read_obj(
    text: str, hand: str, wanted: str
) -> tuple[tuple[list[tuple[float, ...]], ...], list[list[tuple[int, ...]]]]:
    """Return the OBJ's positions (metres), UVs and normals, and the faces of the group @p wanted."""
    positions: list[tuple[float, ...]] = []
    uvs: list[tuple[float, ...]] = []
    normals: list[tuple[float, ...]] = []
    group = ""
    faces: list[list[tuple[int, ...]]] = []
    for number, line in enumerate(text.splitlines(), 1):
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        try:
            if words[0] == "v":
                positions.append(tuple(float(w) * CM for w in words[1:4]))
            elif words[0] == "vt":
                uvs.append(tuple(float(w) for w in words[1:3]))
            elif words[0] == "vn":
                normals.append(tuple(float(w) for w in words[1:4]))
            elif words[0] == "g":
                group = " ".join(words[1:])
            elif words[0] == "f" and wanted in group.split():
                faces.append(_face(words[1:]))
        except ValueError as exc:
            msg = f"{hand}: line {number}: {exc}"
            raise ControllerModelError(msg) from None
    return (positions, uvs, normals), faces


def _face(words: list[str]) -> list[tuple[int, ...]]:
    """Return a face's corners as 0-based (v, vt, vn) indices."""
    corners = [tuple(int(i) - 1 for i in w.split("/")) for w in words]
    if len(corners) < MIN_CORNERS or any(len(c) != CORNER_INDICES or min(c) < 0 for c in corners):
        msg = "a face needs three or more v/vt/vn corners"
        raise ValueError(msg)
    return corners


def _compact(
    sources: tuple[Sequence[tuple[float, ...]], ...], faces: Sequence[Sequence[tuple[int, ...]]], hand: str
) -> Mesh:
    """Return the faces as a mesh of only the vertices they use, fanned into triangles."""
    mesh = Mesh()
    remap: list[dict[int, int]] = [{}, {}, {}]
    # The parser checked each tuple's length; the lists take them as the mesh's types.
    targets: tuple[list[Any], list[Any], list[Any]] = (mesh.positions, mesh.uvs, mesh.normals)
    for corners in faces:
        indexed: list[Any] = []
        for corner in corners:
            out = []
            for k, i in enumerate(corner):
                if i >= len(sources[k]):
                    msg = f"{hand}: a face refers to a missing vertex"
                    raise ControllerModelError(msg)
                if i not in remap[k]:
                    remap[k][i] = len(targets[k])
                    targets[k].append(sources[k][i])
                out.append(remap[k][i])
            indexed.append(tuple(out))
        # A fan: the faces are triangles and convex quads.
        for i in range(1, len(indexed) - 1):
            mesh.triangles.append((indexed[0], indexed[i], indexed[i + 1]))
    return mesh


def connected_parts(mesh: Mesh) -> list[Mesh]:
    """Split triangles by welded position connectivity, without changing any vertex attributes."""
    parents = list(range(len(mesh.positions)))

    def root(index: int) -> int:
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    unique = {}
    for index, point in enumerate(mesh.positions):
        key = tuple(round(v, WELD_DECIMALS) for v in point)
        if key in unique:
            parents[root(index)] = root(unique[key])
        else:
            unique[key] = index
    for triangle in mesh.triangles:
        for corner in triangle[1:]:
            parents[root(corner[0])] = root(triangle[0][0])
    groups: dict[int, list[tuple[tuple[int, int, int], ...]]] = {}
    for triangle in mesh.triangles:
        groups.setdefault(root(triangle[0][0]), []).append(triangle)
    sources = (mesh.positions, mesh.uvs, mesh.normals)
    return [_compact(sources, faces, "component") for faces in sorted(groups.values(), key=len, reverse=True)]


def centre(mesh: Mesh) -> list[float]:
    """Return the bounding-box centre, independent of vertex duplication at UV or normal seams."""
    low, high = bounds(mesh)
    return [round((lo + hi) / 2, WELD_DECIMALS) for lo, hi in zip(low, high)]


def animated_parts(mesh: Mesh, hand: str) -> dict[str, Mesh]:
    """Identify the stock topology; otherwise leave the complete static body intact.

    The three equally sized face buttons are PICO, X/A and Y/B in mirrored x order; MENU is
    smaller. The shell, tracking ring and LED stay stationary. Face order is not significant.
    """
    parts = connected_parts(mesh)
    if tuple(len(part.triangles) for part in parts) != PART_TRIANGLES:
        return {}
    sign = 1 if hand == "left" else -1
    pico, primary, secondary = sorted(parts[5:8], key=lambda part: sign * centre(part)[0])
    stationary = Mesh()
    for part in (parts[0], parts[1], parts[9]):
        offsets = len(stationary.positions), len(stationary.uvs), len(stationary.normals)
        stationary.positions.extend(part.positions)
        stationary.uvs.extend(part.uvs)
        stationary.normals.extend(part.normals)
        stationary.triangles.extend(
            tuple((p + offsets[0], t + offsets[1], n + offsets[2]) for p, t, n in triangle)
            for triangle in part.triangles
        )
    return {
        "body": stationary,
        "thumbstick": parts[2],
        "grip_button": parts[3],
        "trigger": parts[4],
        "button_pico": pico,
        f"button_{'x' if hand == 'left' else 'a'}": primary,
        f"button_{'y' if hand == 'left' else 'b'}": secondary,
        "button_menu": parts[8],
    }


def opening_rim(mesh: Mesh) -> list[tuple[float, float, float]]:
    """Return the open mounting rim, welding seam vertices for edge incidence only."""
    points = [(round(x, WELD_DECIMALS), round(y, WELD_DECIMALS), round(z, WELD_DECIMALS)) for x, y, z in mesh.positions]
    edges: Counter = Counter()
    faces = {tuple(points[p] for p, _, _ in triangle) for triangle in mesh.triangles}
    for triangle in faces:
        for a, b in zip(triangle, (*triangle[1:], triangle[0])):
            if a != b:
                edges[tuple(sorted((a, b)))] += 1
    rim = {point for edge, count in edges.items() if count == 1 for point in edge}
    return sorted(rim or set(points))


def stick_pivot(mesh: Mesh) -> list[float]:
    """Extrapolate the bearing centre from the two lowest circular cross-sections.

    The stock mesh's exposed base is a frustum approximating the bearing. Its bottom edge
    is above the rotation centre; using that edge as the pivot tips the base into the shell.
    Equal sphere radii at both rings give the centre's height without vendor coordinates.
    """
    points = sorted({tuple(round(v, WELD_DECIMALS) for v in point) for point in mesh.positions})
    heights = sorted({point[1] for point in points})
    if len(heights) < BEARING_RINGS:
        msg = "the thumbstick has no two bearing cross-sections"
        raise ControllerModelError(msg)
    x, _, z = centre(mesh)
    y0, y1 = heights[:BEARING_RINGS]
    radii = [
        sum((p[0] - x) ** 2 + (p[2] - z) ** 2 for p in points if p[1] == y) / sum(p[1] == y for p in points)
        for y in (y0, y1)
    ]
    y = (radii[1] + y1 * y1 - radii[0] - y0 * y0) / (2 * (y1 - y0))
    return [x, round(y, WELD_DECIMALS), z]


def grip_hinge(mesh: Mesh, hand: str) -> tuple[list[float], list[float]]:
    """Find the mounting rim's long direction and hinge at its front end.

    The grip pivots about the short direction of its tilted oval opening, not the aim z
    axis. Fit the rim in y/z, then put the hinge at its minimum long-axis projection.
    """
    rim = opening_rim(mesh)
    y, z = (sum(p[k] for p in rim) / len(rim) for k in (1, 2))
    yy = sum((p[1] - y) ** 2 for p in rim)
    zz = sum((p[2] - z) ** 2 for p in rim)
    yz = sum((p[1] - y) * (p[2] - z) for p in rim)
    angle = math.atan2(2 * yz, zz - yy) / 2
    ly, lz = math.sin(angle), math.cos(angle)
    offset = min((p[1] - y) * ly + (p[2] - z) * lz for p in rim)
    pivot = [centre(mesh)[0], round(y + offset * ly, WELD_DECIMALS), round(z + offset * lz, WELD_DECIMALS)]
    sign = 1 if hand == "left" else -1
    return pivot, [0.0, -sign * lz, sign * ly]


def motion_components(parts: dict[str, Mesh], hand: str, name: str) -> dict:
    """Describe the stock parts using SteamVR's native motions, all in the original aim frame.

    See ValveSoftware/openvr/wiki/Render-Model-Reference. Pivots come from the mounting geometry;
    travel is nominal visual feedback. Input paths are the driver's existing PICO controls,
    plus MENU/PICO aliases preserving its handed `system` mapping.
    """
    components: dict[str, dict[str, Any]] = {}
    # SteamVR requires controller_button alongside pressed_path. The extra face button
    # uses its own model bit instead of sharing the handed system button.
    model_buttons = {"x": 7, "a": 7, "y": 1, "b": 1, "menu": EXTRA_MODEL_BUTTON, "pico": EXTRA_MODEL_BUTTON}
    model_buttons["menu" if hand == "left" else "pico"] = 0
    for part, mesh in parts.items():
        components[part] = {
            "filename": f"{name}_{part}.obj",
            "component_local": {
                "origin": [0.0, 0.0, 0.0] if part == "body" else centre(mesh),
                "rotate_xyz": [0.0, 0.0, 0.0],
            },
        }
        if part.startswith("button_"):
            control = part.removeprefix("button_")
            components[part]["motion"] = {
                "type": "translate",
                "pressed_path": f"/input/{control}/click",
                "value_mapping": [0.0, BUTTON_TRAVEL_M],
                "axis": [0.0, -1.0, 0.0],
            }
            components[part]["motion"]["controller_button"] = model_buttons[control]
    trigger, grip, stick = (parts[part] for part in ("trigger", "grip_button", "thumbstick"))
    # The trigger's hinge is at the upper front lip; bbox maxima combine unrelated rear/top points.
    trigger_lip = min(trigger.positions, key=lambda point: point[2])
    components["trigger"]["motion"] = {
        "type": "rotate",
        "controller_axis": 1,
        "controller_axis_component": 0,
        "trigger_path": "/input/trigger",
        "value_mapping": [0.0, TRIGGER_TRAVEL_DEG],
        "pivot": [centre(trigger)[0], trigger_lip[1], trigger_lip[2]],
        "axis": [1.0, 0.0, 0.0],
    }
    grip_pivot, grip_axis = grip_hinge(grip, hand)
    components["grip_button"]["motion"] = {
        "type": "rotate",
        "controller_axis": 2,
        "controller_axis_component": 0,
        "trigger_path": "/input/grip",
        "value_mapping": [0.0, GRIP_TRAVEL_DEG],
        "pivot": grip_pivot,
        "axis": grip_axis,
    }
    components["thumbstick"]["motion"] = {
        "type": "joystick",
        "controller_axis": 0,
        "component_path": "/input/joystick",
        "center": stick_pivot(stick),
        "rotate_xyz": [0.0, 0.0, 0.0],
        "joystick_rotation_x": [STICK_TRAVEL_DEG, -STICK_TRAVEL_DEG],
        "joystick_rotation_y": [STICK_TRAVEL_DEG, -STICK_TRAVEL_DEG],
        "press_translate": [0.0, -BUTTON_TRAVEL_M, 0.0],
    }
    return components


def bounds(mesh: Mesh) -> tuple[list[float], list[float]]:
    """Return the mesh's bounding box: its lowest and highest coordinate on each axis."""
    return (
        [min(p[i] for p in mesh.positions) for i in range(3)],
        [max(p[i] for p in mesh.positions) for i in range(3)],
    )


def validate(meshes: dict[str, Mesh]) -> None:
    """Another firmware may ship other models here: refuse anything that is not this pair."""
    for hand, mesh in meshes.items():
        low, high = bounds(mesh)
        extents = [hi - lo for lo, hi in zip(low, high)]
        if min(extents) < MIN_EXTENT_M or max(extents) > MAX_EXTENT_M:
            msg = f"{hand}: extents {', '.join(f'{e:.3f}' for e in extents)} m, not a controller"
            raise ControllerModelError(msg)
        if any(not 0.0 <= v <= 1.0 for uv in mesh.uvs for v in uv):
            msg = f"{hand}: texture coordinates outside the texture"
            raise ControllerModelError(msg)
    (left_low, left_high), (right_low, right_high) = bounds(meshes["left"]), bounds(meshes["right"])
    mirrored = [-right_high[0], right_low[1], right_low[2]], [-right_low[0], right_high[1], right_high[2]]
    worst = max(abs(a - b) for a, b in zip(left_low + left_high, mirrored[0] + mirrored[1]))
    if worst > MIRROR_TOLERANCE_M:
        msg = f"the left model is not the right one mirrored in x ({worst * 1000:.1f} mm off)"
        raise ControllerModelError(msg)


def _sub(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a: Sequence[float], b: Sequence[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def point_triangle_distance(p: Sequence[float], a: Sequence[float], b: Sequence[float], c: Sequence[float]) -> float:
    """Return the distance from p to the closest point of triangle abc (by the triangle's Voronoi regions)."""
    ab, ac = _sub(b, a), _sub(c, a)
    ap, bp, cp = _sub(p, a), _sub(p, b), _sub(p, c)
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    d3, d4 = _dot(ab, bp), _dot(ac, bp)
    d5, d6 = _dot(ab, cp), _dot(ac, cp)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
    closest: Sequence[float]
    if d1 <= 0 and d2 <= 0:
        closest = a
    elif d3 >= 0 and d4 <= d3:
        closest = b
    elif d6 >= 0 and d5 <= d6:
        closest = c
    elif vc <= 0 and d1 >= 0 and d3 <= 0:
        t = d1 / (d1 - d3)
        closest = tuple(a[i] + t * ab[i] for i in range(3))
    elif vb <= 0 and d2 >= 0 and d6 <= 0:
        t = d2 / (d2 - d6)
        closest = tuple(a[i] + t * ac[i] for i in range(3))
    elif va <= 0 and d4 - d3 >= 0 and d5 - d6 >= 0:
        t = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        closest = tuple(b[i] + t * (c[i] - b[i]) for i in range(3))
    else:
        s = 1.0 / (va + vb + vc)
        v, w = vb * s, vc * s
        closest = tuple(a[i] + ab[i] * v + ac[i] * w for i in range(3))
    return math.dist(p, closest)


def surface_distance(mesh: Mesh, p: Sequence[float]) -> float:
    """Return the distance from p to the closest point of the mesh's surface."""
    return min(point_triangle_distance(p, *(mesh.positions[corner[0]] for corner in tri)) for tri in mesh.triangles)


def led_distances(mesh: Mesh, leds: list[list[float]], lever: list[float]) -> list[float]:
    """Each LED's distance from the surface, taken into the published aim frame as the tracker does."""
    points = [(-m[1] - lever[0], m[0] - lever[1], m[2] - lever[2]) for m in leds]
    return [surface_distance(mesh, p) for p in points]


def check_frame(meshes: dict[str, Mesh], led_models: dict, lever_arms: dict) -> dict[str, float]:
    """Return the worst LED distance per hand; refuse a model whose frame is not the aim frame."""
    worst = {}
    for hand, mesh in meshes.items():
        distances = led_distances(mesh, led_models[hand], lever_arms[hand])
        worst[hand] = max(distances)
        if worst[hand] > LED_SURFACE_TOLERANCE_M:
            msg = f"{hand}: an LED of PICO OS's ring is {worst[hand] * 1000:.1f} mm off the model in the aim frame"
            raise ControllerModelError(msg)
    return worst


def grip_component(transform: dict) -> dict:
    """SteamVR's component_local for PICO OS's aim-to-grip transform (a turn about x only)."""
    w, x, y, z = transform["orientation"]
    if abs(y) > AXIS_TOLERANCE or abs(z) > AXIS_TOLERANCE:
        # SteamVR's order of rotate_xyz is not established here; a turn about one axis needs none.
        msg = "the aim-to-grip rotation is not about x alone"
        raise ControllerModelError(msg)
    angle = math.degrees(2.0 * math.atan2(x, w))
    return {"origin": [round(v, 7) for v in transform["position"]], "rotate_xyz": [round(angle, 4), 0.0, 0.0]}


def surface_normal(mesh: Mesh) -> tuple[float, float]:
    """Return the indicator's outward direction in the y-z plane, a unit vector (y, z)."""
    y = sum(n[1] for n in mesh.normals)
    z = sum(n[2] for n in mesh.normals)
    length = math.hypot(y, z)
    if length == 0.0:
        msg = "the battery indicator has no normal in the y-z plane"
        raise ControllerModelError(msg)
    return y / length, z / length


def status_mesh(power: Mesh) -> Mesh:
    """Return the indicator with UVs on the panel's pips.

    As on the Steam Frame's model, u follows x and v falls towards the wearer, with the top of the
    panel at v = 1.
    """
    ny, nz = surface_normal(power)
    # Along the surface towards the wearer (z back).
    ty, tz = (-nz, ny) if ny >= 0.0 else (nz, -ny)
    across = [p[0] for p in power.positions]
    along = [p[1] * ty + p[2] * tz for p in power.positions]
    if max(across) == min(across) or max(along) == min(along):
        msg = "the battery indicator is not a surface"
        raise ControllerModelError(msg)
    left, top, right, bottom = (edge / STATUS_PANEL_PX for edge in STATUS_BATTERY_PX)
    uvs = []
    for x, s in zip(across, along):
        fx = (x - min(across)) / (max(across) - min(across))
        fs = (s - min(along)) / (max(along) - min(along))
        uvs.append((left + fx * (right - left), 1.0 - (top + fs * (bottom - top))))
    triangles = [tuple((p, p, n) for p, _, n in triangle) for triangle in power.triangles]
    return Mesh(list(power.positions), uvs, list(power.normals), triangles)


def status_component(power: Mesh, filename: str) -> dict:
    """Return SteamVR's `status` component for the battery indicator.

    The mesh is in the model's frame, hidden unless the panel is drawn on it, and its local frame
    is at the indicator's centre with -z outwards, as on the Steam Frame's model.
    """
    ny, nz = surface_normal(power)
    centre = [round(sum(p[k] for p in power.positions) / len(power.positions), 6) for k in range(3)]
    angle = round(math.degrees(math.atan2(ny, -nz)), 4)
    return {
        "filename": filename,
        "component_local": {"origin": centre, "rotate_xyz": [angle, 0.0, 0.0]},
        "visibility": {"default": False},
    }


def write_obj(mesh: Mesh, name: str) -> str:
    """Return the mesh as an OBJ file that uses the material @p name."""
    lines = [
        f"# PICO 4 controller ({name}) from PICO OS, in metres, in the aim frame (x right, y up, z back)",
        f"mtllib {name}.mtl",
        f"o {name}",
        f"usemtl {name}",
    ]
    lines += [f"v {x:.6f} {y:.6f} {z:.6f}" for x, y, z in mesh.positions]
    lines += [f"vt {u:.6f} {v:.6f}" for u, v in mesh.uvs]
    lines += [f"vn {x:.6f} {y:.6f} {z:.6f}" for x, y, z in mesh.normals]
    lines += ["f " + " ".join(f"{p + 1}/{t + 1}/{n + 1}" for p, t, n in tri) for tri in mesh.triangles]
    return "\n".join(lines) + "\n"


def write_mtl(name: str) -> str:
    """Return the material @p name: unlit, its colour from @p name.png."""
    return f"newmtl {name}\nKa 0 0 0\nKd 1 1 1\nKs 0 0 0\nd 1\nillum 1\nmap_Kd {name}.png\n"


def write_json(name: str, grip: dict | None, status: dict, parts: dict | None = None) -> str:
    """Return the render model's JSON: the body, the status panel and the pose components."""
    identity = {"component_local": {"origin": [0.0, 0.0, 0.0], "rotate_xyz": [0.0, 0.0, 0.0]}}
    components = {"body": {"filename": f"{name}.obj", **identity}, "status": status, **(parts or {})}
    for pose in ("tip", "aim", "openxr_aim"):
        components[pose] = identity
    if grip is not None:
        for pose in ("grip", "openxr_grip", "handgrip"):
            components[pose] = {"component_local": grip}
    return json.dumps({"components": components}, indent=1) + "\n"


def convert(
    model_dir: Path,
    output_dir: Path,
    aim_to_grip: dict | None = None,
    led_models: dict | None = None,
    lever_arms: dict | None = None,
) -> dict[str, float]:
    """Write both render models; return the worst LED distance per hand when checked."""
    meshes, powers, textures = {}, {}, {}
    for hand, stem in HANDS.items():
        text = (model_dir / f"{stem}.obj").read_text(encoding="utf-8", errors="replace")
        meshes[hand] = parse_obj(text, hand)
        powers[hand] = parse_obj(text, hand, POWER_GROUP)
        textures[hand] = (model_dir / f"{stem}_idle.png").read_bytes()
        if not textures[hand].startswith(PNG_SIGNATURE):
            msg = f"{hand}: {stem}_idle.png is not a PNG file"
            raise ControllerModelError(msg)
    validate(meshes)
    worst = check_frame(meshes, led_models, lever_arms or {}) if led_models is not None else {}
    grips = {hand: grip_component(aim_to_grip[hand]) for hand in HANDS} if aim_to_grip else {}
    for hand, mesh in meshes.items():
        name = NAME.format(hand=hand)
        target = output_dir / name
        target.mkdir(parents=True, exist_ok=True)
        (target / f"{name}.obj").write_bytes((write_obj(mesh, name)).encode("utf-8"))
        (target / f"{name}.mtl").write_bytes((write_mtl(name)).encode("utf-8"))
        (target / f"{name}.png").write_bytes(textures[hand])
        (target / f"{name}_status.obj").write_bytes((write_obj(status_mesh(powers[hand]), name)).encode("utf-8"))
        status = status_component(powers[hand], f"{name}_status.obj")
        parts = animated_parts(mesh, hand)
        for part, component_mesh in parts.items():
            (target / f"{name}_{part}.obj").write_bytes(write_obj(component_mesh, name).encode("utf-8"))
        components = motion_components(parts, hand, name) if parts else None
        (target / f"{name}.json").write_bytes(write_json(name, grips.get(hand), status, components).encode("utf-8"))
    return worst


def main(argv: list[str] | None = None) -> int:
    """Run the converter's command line."""
    parser = argparse.ArgumentParser(
        prog="python -m devices.phoenix.tooling.controllermodel", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument("model_dir", type=Path, help="PICO OS's Neo3Phx directory (60.obj, 61.obj, *_idle.png)")
    parser.add_argument("output_dir", type=Path, help="receives pico4_controller_left/ and pico4_controller_right/")
    parser.add_argument("--aim-to-grip", type=Path, help="calibration/controllers/aim-to-grip.json")
    parser.add_argument(
        "--check",
        nargs=2,
        type=Path,
        metavar=("LED_JSON", "LEVER_JSON"),
        help="led-models.json and lever-arm.json: verify that the models sit in the aim frame",
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        grip = json.loads(args.aim_to_grip.read_text()) if args.aim_to_grip else None
        leds = levers = None
        if args.check:
            leds, levers = (json.loads(p.read_text()) for p in args.check)
        worst = convert(args.model_dir, args.output_dir, grip, leds, levers)
    except (OSError, ValueError, KeyError) as exc:
        print(f"{args.model_dir}: {exc}", file=sys.stderr)
        return 1
    for hand, distance in worst.items():
        print(f"{hand}: every LED within {distance * 1000:.1f} mm of the model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
