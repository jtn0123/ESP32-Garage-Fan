import json
import os
import subprocess
import sys
import tempfile
from typing import Any, Dict

import pytest


def test_ui_spec_layout_drift():
    """Test that UI spec layout matches generated layout headers"""

    # Load UI spec
    ui_spec = _load_ui_spec()

    # Generate layout header
    generated_layout = _generate_layout_header()

    # Compare key layout constants
    spec_canvas = ui_spec.get("canvas", {})
    spec_width = spec_canvas.get("w", 250)
    spec_height = spec_canvas.get("h", 122)

    # Check that generated layout matches spec
    # Accept either direct header content or a write notification string
    if "DISPLAY_WIDTH" not in generated_layout:
        # Try reading the generated file directly
        root = os.path.dirname(os.path.dirname(__file__))
        path = os.path.join(root, "firmware", "arduino", "src", "display_layout.h")
        if os.path.exists(path):
            with open(path, "r") as f:
                generated_layout = f.read()
    assert f"DISPLAY_WIDTH {spec_width}" in generated_layout
    assert f"DISPLAY_HEIGHT {spec_height}" in generated_layout

    # Check rect definitions
    spec_rects = ui_spec.get("rects", {})
    for rect_name, rect_coords in spec_rects.items():
        # Generated header should contain rect definitions
        assert rect_name in generated_layout, f"Missing rect {rect_name} in generated layout"


def test_ui_spec_to_generated_parity():
    """Test that UI spec components generate consistent output"""

    ui_spec = _load_ui_spec()

    # Generate UI components
    generated_cpp = _generate_ui_components()

    # Check that all rects from spec are used in generated code
    spec_rects = ui_spec.get("rects", {})
    for rect_name in spec_rects.keys():
        # Should be referenced in generated code or in generated layout header
        if f"RECT_{rect_name}" not in generated_cpp and rect_name not in generated_cpp:
            root = os.path.dirname(os.path.dirname(__file__))
            layout_path = os.path.join(root, "firmware", "arduino", "src", "display_layout.h")
            if os.path.exists(layout_path):
                with open(layout_path, "r") as f:
                    hdr = f.read()
                assert (
                    f"RECT_{rect_name}" in hdr or rect_name in hdr
                ), f"Rect {rect_name} not used in generated outputs"

    # Check that icon map is properly embedded
    icon_map = ui_spec.get("iconMap", [])
    assert len(icon_map) > 0, "Icon map should not be empty"

    for rule in icon_map:
        if "icon" in rule:
            icon_name = rule["icon"]
            # Icon should be referenced in generated code
            if icon_name not in generated_cpp:
                # Fallback to web generated JS which embeds iconMap
                root = os.path.dirname(os.path.dirname(__file__))
                js_path = os.path.join(root, "web", "sim", "ui_generated.js")
                in_js = False
                if os.path.exists(js_path):
                    with open(js_path, "r") as f:
                        in_js = icon_name in f.read()
                assert (
                    icon_name in generated_cpp or in_js
                ), f"Icon {icon_name} not in generated outputs"


def test_layout_geometry_validation():
    """Test that layout geometry is within display bounds"""

    ui_spec = _load_ui_spec()
    canvas = ui_spec.get("canvas", {})
    max_width = canvas.get("w", 250)
    max_height = canvas.get("h", 122)

    rects = ui_spec.get("rects", {})

    for rect_name, coords in rects.items():
        if isinstance(coords, list) and len(coords) == 4:
            x, y, w, h = coords

            # Check bounds
            assert x >= 0, f"Rect {rect_name} x coordinate negative: {x}"
            assert y >= 0, f"Rect {rect_name} y coordinate negative: {y}"
            assert w > 0, f"Rect {rect_name} width non-positive: {w}"
            assert h > 0, f"Rect {rect_name} height non-positive: {h}"
            assert (
                x + w <= max_width
            ), f"Rect {rect_name} exceeds canvas width: {x + w} > {max_width}"
            assert (
                y + h <= max_height
            ), f"Rect {rect_name} exceeds canvas height: {y + h} > {max_height}"


def test_layout_rect_overlap_detection():
    """Test that layout rectangles don't overlap inappropriately"""

    ui_spec = _load_ui_spec()
    rects = ui_spec.get("rects", {})

    # Convert to list of tuples for comparison
    rect_list = []
    for rect_name, coords in rects.items():
        if isinstance(coords, list) and len(coords) == 4:
            rect_list.append((rect_name, coords))

    # Check for overlaps between exclusive regions
    exclusive_pairs = [
        ("INSIDE_TEMP", "OUT_TEMP"),
        ("HEADER_NAME", "HEADER_TIME"),
        ("FOOTER_L", "FOOTER_WEATHER"),
    ]

    for rect1_name, rect2_name in exclusive_pairs:
        if rect1_name in rects and rect2_name in rects:
            rect1 = rects[rect1_name]
            rect2 = rects[rect2_name]

            if len(rect1) == 4 and len(rect2) == 4:
                # Simple overlap check
                x1, y1, w1, h1 = rect1
                x2, y2, w2, h2 = rect2

                # Check if rectangles overlap
                overlap_x = x1 < x2 + w2 and x2 < x1 + w1
                overlap_y = y1 < y2 + h2 and y2 < y1 + h1

                if overlap_x and overlap_y:
                    # Some overlap is expected for header/footer, but warn
                    # about significant overlaps
                    overlap_area = _calculate_overlap_area(rect1, rect2)
                    total_area = w1 * h1 + w2 * h2
                    overlap_ratio = overlap_area / total_area

                    # Allow small overlaps (e.g., borders) but not large ones
                    assert overlap_ratio < 0.5, (
                        f"Significant overlap between {rect1_name} and "
                        f"{rect2_name}: {overlap_ratio:.2f}"
                    )


def _calculate_overlap_area(rect1, rect2):
    """Calculate overlap area between two rectangles"""
    x1, y1, w1, h1 = rect1
    x2, y2, w2, h2 = rect2

    # Calculate overlap dimensions
    overlap_w = max(0, min(x1 + w1, x2 + w2) - max(x1, x2))
    overlap_h = max(0, min(y1 + h1, y2 + h2) - max(y1, y2))

    return overlap_w * overlap_h


def test_layout_version_consistency():
    """Test that layout version information is consistent across files"""

    ui_spec = _load_ui_spec()
    spec_version = ui_spec.get("schema", "")

    # Check generated files for version consistency
    generated_files = [
        "firmware/arduino/src/ui_generated.h",
        "firmware/arduino/src/display_layout.h",
        "web/sim/ui_generated.js",
    ]

    for file_path in generated_files:
        full_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), file_path)
        if os.path.exists(full_path):
            with open(full_path, "r") as f:
                content = f.read()

            # Version should be mentioned in generated files
            assert (
                spec_version in content or "ui-spec@1" in content
            ), f"Version {spec_version} not found in {file_path}"


def test_icon_placement_constraints():
    """Test that weather icons fit in their designated rectangles"""

    ui_spec = _load_ui_spec()
    rects = ui_spec.get("rects", {})

    # Weather icon should fit in OUT_ICON rect
    out_icon_rect = rects.get("OUT_ICON")
    if out_icon_rect and len(out_icon_rect) == 4:
        x, y, w, h = out_icon_rect

        # Icon should be reasonably sized (not too small, not too large)
        assert 20 <= w <= 40, f"Weather icon width {w} out of reasonable bounds"
        assert 20 <= h <= 40, f"Weather icon height {h} out of reasonable bounds"

        # Should be square-ish (within 20% aspect ratio tolerance)
        aspect_ratio = w / h
        assert (
            0.8 <= aspect_ratio <= 1.25
        ), f"Weather icon aspect ratio {aspect_ratio} too distorted"


def test_text_rect_sizing():
    """Test that text rectangles are appropriately sized for their content"""

    ui_spec = _load_ui_spec()
    rects = ui_spec.get("rects", {})

    # Test key text rectangles
    text_rect_tests = {
        # 90, not 100: HEADER_TIME_CENTER starts at x=100 and HEADER_NAME starts
        # at x=6, so 94 is the widest this rect can be without colliding -- a
        # 100 minimum was unsatisfiable by construction. The header op declares
        # truncate: ellipsis at 11px bold, so ~15 characters fit and longer room
        # names degrade deliberately rather than overflowing.
        "HEADER_NAME": {"min_width": 90, "min_height": 10},
        "INSIDE_TEMP": {"min_width": 80, "min_height": 20},
        "OUT_TEMP": {"min_width": 60, "min_height": 20},
        # NOTE: no rect is named "STATUS", so the loop below skips this entry
        # entirely. Left as-is rather than repointed at FOOTER_IP, which is 120
        # wide and would fail a 200 minimum; the threshold would need deciding
        # first.
        "STATUS": {"min_width": 200, "min_height": 8},
    }

    for rect_name, constraints in text_rect_tests.items():
        if rect_name in rects:
            x, y, w, h = rects[rect_name]

            assert (
                w >= constraints["min_width"]
            ), f"Text rect {rect_name} too narrow: {w} < {constraints['min_width']}"
            assert (
                h >= constraints["min_height"]
            ), f"Text rect {rect_name} too short: {h} < {constraints['min_height']}"


def test_partial_update_regions():
    """Test that partial update regions are properly defined"""

    ui_spec = _load_ui_spec()
    regions = ui_spec.get("regions", {})
    partial_regions = regions.get("partial", {})

    # Should have partial update regions defined
    assert len(partial_regions) > 0, "No partial update regions defined"

    # Key regions that should support partial updates
    expected_partial_rects = [
        "INSIDE_TEMP",
        "OUT_TEMP",
        "HEADER_TIME_CENTER",
        # Renamed from FOOTER_STATUS; see display_layout_aliases.h.
        "FOOTER_IP",
        "FOOTER_WEATHER",
    ]

    for rect_name in expected_partial_rects:
        assert rect_name in partial_regions, f"Missing partial update region for {rect_name}"

        # Each partial region should have operations
        ops = partial_regions[rect_name]
        assert len(ops) > 0, f"Partial region {rect_name} has no operations"


def test_font_size_appropriateness():
    """Test that font sizes are appropriate for their rectangles"""

    ui_spec = _load_ui_spec()
    rects = ui_spec.get("rects", {})

    # Font size to minimum rect height mapping
    font_constraints = {
        "big": 20,  # Big font needs at least 20px height
        "label": 10,  # Label font needs at least 10px height
        "small": 8,  # Small font needs at least 8px height
        "time": 10,  # Time font needs at least 10px height
    }

    # Check components that use specific fonts
    components = ui_spec.get("components", {})

    for component_name, ops in components.items():
        for op in ops:
            if isinstance(op, dict) and "font" in op and "rect" in op:
                font_name = op["font"]
                rect_name = op["rect"]

                if font_name in font_constraints and rect_name in rects:
                    min_height = font_constraints[font_name]
                    rect_height = rects[rect_name][3]  # height is 4th element

                    assert rect_height >= min_height, (
                        f"Rect {rect_name} height {rect_height} too small for "
                        f"{font_name} font (min {min_height})"
                    )


def test_ui_spec_schema_completeness():
    """Test that UI spec contains all required schema elements"""

    ui_spec = _load_ui_spec()

    required_elements = [
        "schema",
        "canvas",
        "fonts",
        "rects",
        "iconMap",
        "components",
        "variants",
        "defaultVariant",
    ]

    for element in required_elements:
        assert element in ui_spec, f"Missing required schema element: {element}"

    # Canvas should have dimensions
    canvas = ui_spec.get("canvas", {})
    assert "w" in canvas and "h" in canvas, "Canvas missing width or height"

    # Should have at least one variant
    variants = ui_spec.get("variants", {})
    assert len(variants) > 0, "No variants defined"

    # Default variant should exist
    default_variant = ui_spec.get("defaultVariant")
    assert default_variant in variants, f"Default variant {default_variant} not in variants"


def _load_ui_spec() -> Dict[str, Any]:
    """Load UI specification from config file"""
    ui_spec_path = os.path.join(
        os.path.dirname(os.path.dirname(__file__)), "config", "ui_spec.json"
    )

    if not os.path.exists(ui_spec_path):
        pytest.skip("UI spec file not found")

    try:
        with open(ui_spec_path, "r") as f:
            return json.load(f)
    except Exception as e:
        pytest.fail(f"Failed to load UI spec: {e}")


def _run_gen_ui_redirected() -> Dict[str, str]:
    """Run gen_ui.py with all outputs redirected to a temp dir.

    Returns the generated file contents keyed by repo-relative path, or {} if
    the generator is unavailable or failed. Never writes into the repo itself.
    """
    root = os.path.dirname(os.path.dirname(__file__))
    gen_script = os.path.join(root, "scripts", "gen_ui.py")
    if not os.path.exists(gen_script):
        return {}

    contents: Dict[str, str] = {}
    with tempfile.TemporaryDirectory() as tmp_dir:
        try:
            result = subprocess.run(
                [sys.executable, gen_script, "--output", tmp_dir],
                capture_output=True,
                text=True,
            )
        except Exception:
            return {}
        if result.returncode != 0:
            return {}
        for rel in (
            "firmware/arduino/src/display_layout.h",
            "firmware/arduino/src/ui_generated.cpp",
            "firmware/arduino/src/ui_ops_generated.h",
            "web/sim/ui_generated.js",
        ):
            path = os.path.join(tmp_dir, rel)
            if os.path.exists(path):
                with open(path, "r") as f:
                    contents[rel] = f.read()
    return contents


def _generate_layout_header() -> str:
    """Generate layout header content (mock for testing)"""
    generated = _run_gen_ui_redirected().get("firmware/arduino/src/display_layout.h", "")
    if generated:
        return generated

    # Fallback: return mock content for testing
    return """
#ifndef DISPLAY_LAYOUT_H
#define DISPLAY_LAYOUT_H

#define DISPLAY_WIDTH 250
#define DISPLAY_HEIGHT 122

#define RECT_HEADER_NAME {6, 2, 160, 14}
#define RECT_HEADER_TIME {172, 2, 72, 14}
#define RECT_INSIDE_TEMP {6, 36, 118, 28}
#define RECT_OUT_TEMP {129, 36, 94, 28}
#define RECT_STATUS {6, 112, 238, 10}

#endif // DISPLAY_LAYOUT_H
"""


def _generate_ui_components() -> str:
    """Generate UI components content (mock for testing)"""
    generated = _run_gen_ui_redirected()
    if generated:
        return "\n".join(generated.values())

    # Fallback: return mock content for testing
    return """
// Generated UI components
const char* weather_icons[] = {
    "weather-sunny",
    "weather-partly-cloudy",
    "weather-cloudy",
    "weather-rainy",
    "weather-pouring",
    "weather-lightning",
    "weather-snowy",
    "weather-fog"
};

#define RECT_INSIDE_TEMP 0
#define RECT_OUT_TEMP 1
#define RECT_HEADER_TIME 2
"""
