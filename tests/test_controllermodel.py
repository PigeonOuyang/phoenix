# SPDX-License-Identifier: Unlicense
"""The controllers' render model (synthetic data here)."""

from __future__ import annotations

import json
import math
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from typing import Any

from devices.phoenix.tooling import controllermodel

PNG = controllermodel.PNG_SIGNATURE + b"synthetic texture"


def box_obj(sign: float = 1.0, units: str = "centimeters", extra: str = "") -> str:
    """Return a synthetic model: a 6 x 8 x 10 cm box of quads, its corner x scaled by `sign`.

    A battery-indicator group must not appear in the output. Not the real model.
    """
    corners = [(sx * 3 * sign + 1 * sign, sy * 4, sz * 5) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    quads = [(1, 2, 4, 3), (5, 7, 8, 6), (1, 5, 6, 2), (3, 4, 8, 7), (1, 3, 7, 5), (2, 6, 8, 4)]
    lines = [f"# This file uses {units} as units for non-parametric coordinates.", "", "mtllib x.mtl", "g default"]
    lines += [f"v {x} {y} {z}" for x, y, z in corners]
    lines += [
        "vt 0 0",
        "vt 1 0",
        "vt 1 1",
        "vt 0 1",
        "vn 0 0 1",
        "s 1",
        "g Controller PICO4_Controller_Left",
        "usemtl lambert2SG",
    ]
    lines += ["f " + " ".join(f"{v}/{k + 1}/1" for k, v in enumerate(q)) for q in quads]
    lines += [
        "g default",
        "v 0 0 0",
        "g PICO4_Controller_Left Power",
        "usemtl initialShadingGroup",
        "f 9/1/1 1/2/1 2/3/1",
    ]
    return "\n".join(lines) + "\n" + extra


def model_dir(root: Path, left: str | None = None, right: str | None = None, png: bytes = PNG) -> Path:
    """Write a model directory like PICO OS's: both hands' OBJ files and their textures."""
    d = root / "Neo3Phx"
    d.mkdir()
    (d / "60.obj").write_text(left if left is not None else box_obj(1.0))
    (d / "61.obj").write_text(right if right is not None else box_obj(-1.0))
    for stem in ("60", "61"):
        (d / f"{stem}_idle.png").write_bytes(png)
    return d


def parse_written_obj(text: str) -> tuple[list[tuple[float, ...]], list[list[int]]]:
    """Return an OBJ file's positions and faces (position indices)."""
    positions, faces = [], []
    for line in text.splitlines():
        words = line.split()
        if words and words[0] == "v":
            positions.append(tuple(map(float, words[1:])))
        elif words and words[0] == "f":
            faces.append([int(w.split("/")[0]) for w in words[1:]])
    return positions, faces


def disconnected_mesh(sign: float = 1.0) -> controllermodel.Mesh:
    """Synthetic tetrahedra with the stock topology's counts; no vendor vertices or texture data."""
    mesh = controllermodel.Mesh()
    counts = (2234, 650, 606, 224, 215, 160, 160, 160, 96, 14)
    for part, count in enumerate(counts):
        # The equal-count pieces have deliberately scrambled x order.
        x = {5: 0.015, 6: -0.015, 7: 0.0}.get(part, part * 0.008 - 0.04)
        y, z = part * 0.008 - 0.04, part * 0.01 - 0.05
        points = [(x, y, z), (x + 0.004, y, z), (x, y + 0.004, z), (x, y, z + 0.004)]
        offset = len(mesh.positions)
        mesh.positions.extend((px * sign, py, pz) for px, py, pz in points)
        mesh.uvs.extend([(0.0, 0.0), (0.1, 0.2), (0.3, 0.4), (0.5, 0.6)])
        mesh.normals.extend([(1.0, 0.0, 0.0)] * 4)
        faces = [(0, 1, 2), (0, 3, 1), (1, 3, 2), (2, 3, 0)]
        mesh.triangles.extend(tuple((offset + v, offset + v, offset + v) for v in faces[i % 4]) for i in range(count))
    return mesh


def triangle_attributes(mesh: controllermodel.Mesh) -> Counter:
    """Count triangles by all their corner attributes, independent of compacted index numbers."""
    return Counter(
        tuple((mesh.positions[p], mesh.uvs[t], mesh.normals[n]) for p, t, n in triangle) for triangle in mesh.triangles
    )


def mesh_obj(mesh: controllermodel.Mesh) -> str:
    """Return a synthetic centimetre OBJ with a separate battery strip."""
    lines = ["# This file uses centimeters as units.", "g Controller"]
    lines += [f"v {x * 100} {y * 100} {z * 100}" for x, y, z in mesh.positions]
    lines += [f"vt {u} {v}" for u, v in mesh.uvs]
    lines += [f"vn {x} {y} {z}" for x, y, z in mesh.normals]
    lines += ["f " + " ".join(f"{p + 1}/{t + 1}/{n + 1}" for p, t, n in tri) for tri in mesh.triangles]
    position, normal = len(mesh.positions) + 1, len(mesh.normals) + 1
    lines += ["v -1 1 1", "v 1 1 1", "v 0 1 2", "vn 0 1 1", "g Power"]
    lines += [f"f {position}/1/{normal} {position + 1}/2/{normal} {position + 2}/3/{normal}"]
    return "\n".join(lines) + "\n"


class AnimationTests(unittest.TestCase):
    def test_seam_welding_preserves_every_triangle_and_all_attributes(self):
        mesh = disconnected_mesh()
        p = mesh.triangles[0][0][0]
        mesh.positions.append(mesh.positions[p])
        mesh.uvs.append((0.9, 0.9))
        mesh.normals.append((0.0, 1.0, 0.0))
        mesh.triangles[0] = (
            (len(mesh.positions) - 1, len(mesh.uvs) - 1, len(mesh.normals) - 1),
            *mesh.triangles[0][1:],
        )
        parts = controllermodel.animated_parts(mesh, "left")
        self.assertEqual(len(parts), 8)
        self.assertEqual(len(parts["body"].triangles), 2898)
        combined = Counter()
        for part in parts.values():
            combined.update(triangle_attributes(part))
        self.assertEqual(combined, triangle_attributes(mesh))
        self.assertEqual(sum(combined.values()), 4519)

    def test_button_mapping_uses_mirrored_geometry_not_face_order(self):
        for hand, sign, primary, secondary in (("left", 1, "x", "y"), ("right", -1, "a", "b")):
            mesh = disconnected_mesh(sign)
            mesh.triangles.reverse()
            parts = controllermodel.animated_parts(mesh, hand)
            xs = [
                sign * controllermodel.centre(parts[part])[0]
                for part in ("button_pico", f"button_{primary}", f"button_{secondary}")
            ]
            self.assertEqual(xs, sorted(xs))
            components = controllermodel.motion_components(parts, hand, "synthetic")
            for control in ("pico", "menu", primary, secondary):
                motion = components[f"button_{control}"]["motion"]
                self.assertEqual(motion["pressed_path"], f"/input/{control}/click")
                self.assertEqual(motion["value_mapping"][0], 0.0)
            self.assertEqual(components[f"button_{primary}"]["motion"]["controller_button"], 7)
            self.assertEqual(components[f"button_{secondary}"]["motion"]["controller_button"], 1)
            system = "menu" if hand == "left" else "pico"
            extra = "pico" if hand == "left" else "menu"
            self.assertEqual(components[f"button_{system}"]["motion"]["controller_button"], 0)
            self.assertEqual(components[f"button_{extra}"]["motion"]["controller_button"], 8)
            self.assertNotEqual(
                components[f"button_{extra}"]["motion"]["controller_button"],
                components[f"button_{system}"]["motion"]["controller_button"],
            )
            self.assertEqual(components["trigger"]["motion"]["trigger_path"], "/input/trigger")
            self.assertEqual(components["grip_button"]["motion"]["trigger_path"], "/input/grip")
            self.assertAlmostEqual(sum(v * v for v in components["grip_button"]["motion"]["axis"]), 1.0)
            self.assertEqual(components["thumbstick"]["motion"]["component_path"], "/input/joystick")
            self.assertEqual(components["thumbstick"]["motion"]["press_translate"], [0.0, -0.001, 0.0])

    def test_stick_bearing_centre_is_below_the_exposed_bottom_ring(self):
        mesh = controllermodel.Mesh()
        centre = (0.02, 0.002, -0.03)
        sphere_radius = 0.012
        for y in (0.006, 0.009):
            radius = math.sqrt(sphere_radius**2 - (y - centre[1]) ** 2)
            mesh.positions.extend(
                (centre[0] + radius * math.cos(i * math.tau / 16), y, centre[2] + radius * math.sin(i * math.tau / 16))
                for i in range(16)
            )
        mesh.positions.extend(mesh.positions[:8])  # duplicated seam vertices
        pivot = controllermodel.stick_pivot(mesh)
        for actual, expected in zip(pivot, centre):
            self.assertAlmostEqual(actual, expected, places=6)
        self.assertLess(pivot[1], min(point[1] for point in mesh.positions))

    def test_grip_hinge_follows_the_tilted_opening_and_mirrors_the_motion(self):
        mesh = controllermodel.Mesh()
        tilt = math.radians(35)
        ly, lz = math.sin(tilt), math.cos(tilt)
        for i in range(32):
            angle = i * math.tau / 32
            along, across = 0.02 * math.cos(angle), 0.008 * math.sin(angle)
            mesh.positions.append((0.003, along * ly + across * lz, along * lz - across * ly))
        left_pivot, left_axis = controllermodel.grip_hinge(mesh, "left")
        right_pivot, right_axis = controllermodel.grip_hinge(mesh, "right")
        self.assertEqual(left_pivot, right_pivot)
        self.assertEqual(right_axis, [-v for v in left_axis])
        self.assertAlmostEqual(left_axis[1], -lz, places=4)
        self.assertAlmostEqual(left_axis[2], ly, places=4)
        self.assertAlmostEqual(left_pivot[1], -0.02 * ly, places=6)
        self.assertAlmostEqual(left_pivot[2], -0.02 * lz, places=6)

    def test_unknown_topology_keeps_the_static_model(self):
        mesh = disconnected_mesh()
        mesh.triangles.pop()
        self.assertEqual(controllermodel.animated_parts(mesh, "left"), {})

    def test_conversion_keeps_the_texture_static_fallback_and_pose_components(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = model_dir(root, mesh_obj(disconnected_mesh()), mesh_obj(disconnected_mesh(-1)))
            grip = {"position": [0.0, 0.01, 0.02], "orientation": [1.0, 0.0, 0.0, 0.0]}
            controllermodel.convert(source, root / "out", {"left": grip, "right": grip})
            for hand in ("left", "right"):
                name = f"pico4_controller_{hand}"
                target = root / "out" / name
                components = json.loads((target / f"{name}.json").read_text())["components"]
                self.assertEqual(components["body"]["filename"], f"{name}_body.obj")
                self.assertEqual(len(parse_written_obj((target / f"{name}.obj").read_text())[1]), 4519)
                self.assertEqual((target / f"{name}.png").read_bytes(), PNG)
                self.assertEqual(components["grip"]["component_local"]["origin"], [0.0, 0.01, 0.02])
                self.assertEqual(components["aim"]["component_local"]["origin"], [0.0, 0.0, 0.0])
                self.assertEqual(components["status"]["visibility"], {"default": False})
                self.assertEqual(sum("motion" in part for part in components.values()), 7)
                self.assertTrue(
                    all((target / part["filename"]).is_file() for part in components.values() if "filename" in part)
                )


class ControllerModelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_writes_both_render_models_in_metres_triangulated_without_the_battery_indicator(self):
        out = self.root / "out"
        controllermodel.convert(model_dir(self.root), out)
        for hand, sign in (("left", 1.0), ("right", -1.0)):
            name = f"pico4_controller_{hand}"
            d = out / name
            self.assertEqual(
                sorted(p.name for p in d.iterdir()),
                sorted([f"{name}_status.obj"] + [f"{name}.{ext}" for ext in ("json", "mtl", "obj", "png")]),
            )
            positions, faces = parse_written_obj((d / f"{name}.obj").read_text())
            self.assertEqual(len(positions), 8)  # the indicator's vertex is gone
            self.assertEqual(len(faces), 12)
            self.assertTrue(all(len(f) == 3 for f in faces))
            xs = sorted({round(p[0], 6) for p in positions})
            self.assertEqual(xs, sorted([round(sign * -0.02, 6), round(sign * 0.04, 6)]))
            self.assertEqual(sorted({round(p[2], 6) for p in positions}), [-0.05, 0.05])
            self.assertIn(f"map_Kd {name}.png", (d / f"{name}.mtl").read_text())
            self.assertEqual((d / f"{name}.png").read_bytes(), PNG)
            self.assertNotIn(b"\r\n", (d / f"{name}.obj").read_bytes())
            components = json.loads((d / f"{name}.json").read_text())["components"]
            self.assertEqual(components["body"]["filename"], f"{name}.obj")
            self.assertEqual(components["aim"]["component_local"]["origin"], [0.0, 0.0, 0.0])
            self.assertNotIn("grip", components)

    def test_the_indicator_becomes_steamvrs_status_component_on_the_panels_pips(self):
        # A strip across x, tilted: its near edge is lower and further back, its normal up and back.
        strip = ["v -0.5 0.5 1.0", "v 0.5 0.5 1.0", "v 0.5 0.1 1.3", "v -0.5 0.1 1.3"]
        obj = box_obj(1.0).replace(
            "g PICO4_Controller_Left Power", "\n".join([*strip, "vn 0 0.6 0.8", "g PICO4_Controller_Left Power"])
        )
        obj = obj.replace("f 9/1/1 1/2/1 2/3/1", "f 10/1/2 11/2/2 12/3/2\nf 10/1/2 12/3/2 13/4/2")
        out = self.root / "out"
        controllermodel.convert(model_dir(self.root, left=obj), out)
        name = "pico4_controller_left"
        component = json.loads((out / name / f"{name}.json").read_text())["components"]["status"]
        self.assertEqual(component["filename"], f"{name}_status.obj")
        self.assertEqual(component["visibility"], {"default": False})
        self.assertEqual(component["component_local"]["origin"], [0.0, 0.003, 0.0115])
        self.assertAlmostEqual(
            component["component_local"]["rotate_xyz"][0], math.degrees(math.atan2(0.6, -0.8)), places=3
        )
        lines = (out / name / f"{name}_status.obj").read_text().splitlines()
        positions = [tuple(map(float, line.split()[1:])) for line in lines if line.startswith("v ")]
        uvs = [tuple(map(float, line.split()[1:])) for line in lines if line.startswith("vt ")]
        by_corner = dict(zip(positions, uvs))
        left, top, right, bottom = 23 / 256, 172 / 256, 233 / 256, 197 / 256
        for corner, (u, v) in (
            ((-0.005, 0.005, 0.01), (left, 1 - top)),
            ((0.005, 0.005, 0.01), (right, 1 - top)),
            ((0.005, 0.001, 0.013), (right, 1 - bottom)),
            ((-0.005, 0.001, 0.013), (left, 1 - bottom)),
        ):
            self.assertAlmostEqual(by_corner[corner][0], u, places=5)
            self.assertAlmostEqual(by_corner[corner][1], v, places=5)

    def test_a_model_without_the_indicator_is_refused(self):
        obj = box_obj(1.0).replace("g PICO4_Controller_Left Power", "g PICO4_Controller_Left Other")
        with self.assertRaises(controllermodel.ControllerModelError):
            controllermodel.convert(model_dir(self.root, left=obj), self.root / "out")

    def test_grip_components_follow_the_aim_to_grip_transform(self):
        half = math.radians(34.0) / 2
        transform = {"position": [0.0016, 0.0141, 0.0491], "orientation": [math.cos(half), math.sin(half), 0.0, 0.0]}
        out = self.root / "out"
        controllermodel.convert(model_dir(self.root), out, {"left": transform, "right": transform})
        components = json.loads((out / "pico4_controller_right" / "pico4_controller_right.json").read_text())[
            "components"
        ]
        for pose in ("grip", "openxr_grip", "handgrip"):
            local = components[pose]["component_local"]
            self.assertEqual(local["origin"], [0.0016, 0.0141, 0.0491])
            self.assertAlmostEqual(local["rotate_xyz"][0], 34.0, places=3)
            self.assertEqual(local["rotate_xyz"][1:], [0.0, 0.0])
        tilted = dict(transform, orientation=[math.cos(half), 0.0, math.sin(half), 0.0])
        (self.root / "tilted").mkdir()
        with self.assertRaises(controllermodel.ControllerModelError):
            controllermodel.convert(model_dir(self.root / "tilted"), out, {"left": tilted, "right": tilted})

    def test_check_accepts_leds_on_the_surface_in_the_aim_frame_and_refuses_others(self):
        lever = {"left": [0.001, -0.002, 0.003], "right": [-0.001, -0.002, 0.003]}

        def led_in_m(aim_point: tuple[float, float, float], hand: str) -> list[float]:
            # The tracker's model frame M: aim = (-m.y, m.x, m.z) - lever.
            x, y, z = (a + offset for a, offset in zip(aim_point, lever[hand]))
            return [y, -x, z]

        # Points on the faces of each box (left box x from -0.02 to 0.04 m, y +-0.04, z +-0.05).
        on_left = [(0.04, 0.0, 0.0), (0.0, 0.04, 0.01), (0.01, -0.01, 0.05)]
        on_right = [(-x, y, z) for x, y, z in on_left]
        leds = {"left": [led_in_m(p, "left") for p in on_left], "right": [led_in_m(p, "right") for p in on_right]}
        worst = controllermodel.convert(model_dir(self.root), self.root / "out", None, leds, lever)
        self.assertLess(max(worst.values()), 1e-9)
        inside = {hand: [*pts, led_in_m((0.01, 0.0, 0.0), hand)] for hand, pts in leds.items()}
        (self.root / "inside").mkdir()
        with self.assertRaises(controllermodel.ControllerModelError):
            controllermodel.convert(model_dir(self.root / "inside"), self.root / "out2", None, inside, lever)

    def test_refuses_what_is_not_this_pair_of_models(self):
        cases: dict[str, dict[str, Any]] = {
            "metres": {"left": box_obj(1.0, units="meters")},
            "no body": {"left": "# This file uses centimeters as units\nv 0 0 0\n"},
            "not mirrored": {"right": box_obj(1.0)},
            "bad face": {"left": box_obj(1.0, extra="g Controller X\nf 1/1/1 99/1/1 2/1/1\n")},
            "not a png": {"png": b"GIF89a"},
        }
        for i, (name, kwargs) in enumerate(cases.items()):
            root = self.root / str(i)
            root.mkdir()
            with self.subTest(name), self.assertRaises(controllermodel.ControllerModelError):
                controllermodel.convert(model_dir(root, **kwargs), root / "out")

    def test_main_reports_errors(self):
        self.assertEqual(controllermodel.main([str(self.root / "missing"), str(self.root / "out")]), 1)


if __name__ == "__main__":
    unittest.main()
