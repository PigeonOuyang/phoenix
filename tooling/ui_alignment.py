# SPDX-License-Identifier: Unlicense
"""Measure centered glyph pixels from the actual native renderer and fonts."""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops  # ty: ignore[import-not-found]  # Pillow is an optional renderer-check dependency.

MAX_VERTICAL_ERROR_PX = 0.5


def main() -> None:  # noqa: C901, PLR0912, PLR0915  # Keep the renderer's ordered geometry checks in one pass.
    """Measure the native renderer's labels without relaxing pixel tolerances."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    source = json.loads((args.input / "alignment.json").read_text(encoding="utf-8"))
    if source.get("mode") != "offline_zero_disparity_layout":
        msg = "alignment measurements require explicitly unshifted renderer output"
        raise RuntimeError(msg)
    results = []
    separator_checks = 0
    single_line_checks = 0
    optical_checks = []
    run_checks = []
    gap_checks = []
    for case in source["cases"]:
        with Image.open(args.input / case["image"]) as file:
            image = file.convert("RGB")
        for x in (540, 1080, 1740):
            if image.getpixel((x, 487)) != (77, 80, 85):
                msg = f"status separator interrupted: {case['image']} x={x}"
                raise RuntimeError(msg)
            separator_checks += 1
        for label in case.get("single_line", []):
            if not 0 < label["advance_px"] <= label["available_px"]:
                msg = f"single line overflow: {case['image']} {label}"
                raise RuntimeError(msg)
            single_line_checks += 1
        previous = None
        for label in case.get("status_ink", []) + case.get("status_runs", []):
            x, y, width, height = label["bounds"]
            if label.get("previous_icon_gap", 0):
                # Tight packing intentionally lets the battery occupy unused
                # leading space in the right-aligned percentage container.
                # Measure the actual percentage run rather than that container.
                matches = [
                    run
                    for run in case.get("status_runs", [])
                    if run["value"] == label["value"] and run["bounds"][1] == y and x <= run["bounds"][0] <= x + width
                ]
                if len(matches) != 1:
                    msg = "percentage must have one measured font run"
                    raise RuntimeError(msg)
                x, y, width, height = matches[0]["bounds"]
            crop = (round(x), round(y), round(x + width), round(y + height))
            region = image.crop(crop)
            # Status containers retain clear top padding. A full battery can
            # occupy most of its rectangle, so its fill is not the background.
            background = region.getpixel((0, 0))
            contrast = max(abs(a - b) for a, b in zip(label["color"], background))
            difference = ImageChops.difference(region, Image.new("RGB", region.size, background))
            channels = difference.split()
            distance = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
            mask = distance.point([255 if value >= max(1, contrast / 2) else 0 for value in range(256)])
            box = mask.getbbox()
            if box is None:
                msg = f"no status ink: {case['image']} {label['value']}"
                raise RuntimeError(msg)
            actual_height = box[3] - box[1]
            dy = (box[1] + box[3]) / 2 + crop[1] - (y + height / 2)
            # Dash placeholders retain the same number font and center but have
            # intentionally shorter ink than digits.
            placeholder = label["value"].strip(" -") == ""
            record = {
                "image": case["image"],
                "value": label["value"],
                "height_px": actual_height,
                "target_height_px": label["target_height"],
                "vertical_error_px": dy,
                "placeholder": placeholder,
            }
            record["ink_left_px"] = crop[0] + box[0]
            record["ink_right_px"] = crop[0] + box[2]
            if "font_face" in label:
                record["font_face"] = label["font_face"]
                run_checks.append(record)
            else:
                optical_checks.append(record)
                if label.get("previous_icon_gap", 0):
                    if previous is None or not previous["value"].startswith("battery:"):
                        msg = "percentage gap is missing its battery icon"
                        raise RuntimeError(msg)
                    gap = record["ink_left_px"] - previous["ink_right_px"]
                    target = label["previous_icon_gap"]
                    if abs(gap - target) > 1:
                        msg = f"battery percentage gap {gap}, expected {target}"
                        raise RuntimeError(msg)
                    gap_checks.append({"image": case["image"], "gap_px": gap, "target_px": target})
                previous = record
            if abs(dy) > MAX_VERTICAL_ERROR_PX or (not placeholder and abs(actual_height - label["target_height"]) > 1):
                msg = f"status ink height/alignment: {record}"
                raise RuntimeError(msg)
        for label in case["labels"]:
            x, y, width, height = label["bounds"]
            # Exclude the fixed back-arrow and container corners. Action labels
            # already have symmetric icon padding in the native scene.
            inset = 64 if label["back"] else 8
            vertical_inset = min(8, height / 8)
            crop = (
                round(x + inset),
                round(y + vertical_inset),
                round(x + width - inset),
                round(y + height - vertical_inset),
            )
            region = image.crop(crop)
            # Normalize antialias coverage to the actual background. A fixed
            # distance from ink selects different glyph edges for light and dark
            # labels, producing color-dependent one-pixel false offsets.
            background = max(region.getcolors(region.width * region.height), key=lambda item: item[0])[1]
            contrast = max(abs(a - b) for a, b in zip(label["color"], background))
            if not contrast:
                msg = f"label has no background contrast: {label['text']}"
                raise RuntimeError(msg)
            difference = ImageChops.difference(region, Image.new("RGB", region.size, background))
            channels = difference.split()
            distance = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
            mask = distance.point([255 if value >= contrast / 2 else 0 for value in range(256)])
            box = mask.getbbox()
            if box is None:
                msg = f"no glyph pixels: {case['image']} {label['text']}"
                raise RuntimeError(msg)
            dx = (box[0] + box[2]) / 2 + crop[0] - (x + width / 2)
            dy = (box[1] + box[3]) / 2 + crop[1] - (y + height / 2)
            record = {"image": case["image"], "text": label["text"], "horizontal_error_px": dx, "vertical_error_px": dy}
            results.append(record)
            # FreeType hinting of Latin labels can move the half-coverage
            # edge by one pixel from the reported ink extents.
            tolerance = 1 if label["text"].isascii() else 0.5
            if abs(dx) > tolerance or abs(dy) > MAX_VERTICAL_ERROR_PX:
                msg = f"glyph centering exceeds pixel tolerance: {record}"
                raise RuntimeError(msg)
    if not results:
        msg = "renderer did not provide any aligned labels"
        raise RuntimeError(msg)
    report = {
        "passed": True,
        "mode": source["mode"],
        "labels": len(results),
        "separator_checks": separator_checks,
        "single_line_checks": single_line_checks,
        "status_optical_checks": len(optical_checks),
        "status_optical_measurements": optical_checks,
        "status_font_run_checks": len(run_checks),
        "status_font_run_measurements": run_checks,
        "battery_gap_checks": gap_checks,
        "maximum_horizontal_error_px": max(abs(item["horizontal_error_px"]) for item in results),
        "maximum_vertical_error_px": max(abs(item["vertical_error_px"]) for item in results),
        "measurements": results,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(
        f"ALIGNMENT_OK labels={len(results)} "
        f"dx={report['maximum_horizontal_error_px']} dy={report['maximum_vertical_error_px']}"
    )


if __name__ == "__main__":
    main()
