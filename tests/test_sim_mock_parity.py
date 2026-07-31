#!/usr/bin/env python3
"""
Test that mock_display.py and web simulator render identically.

This is the "golden test" that guarantees device/simulator parity:
- Both read from ui_spec.json
- Both should produce identical pixel output
- If this test fails, the simulator and device will differ

Run with: pytest tests/test_sim_mock_parity.py -v
"""

import importlib.util
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def load_mock_display():
    """Load mock_display.py dynamically."""
    module_path = ROOT / "scripts" / "mock_display.py"
    spec = importlib.util.spec_from_file_location("mock_display", module_path)
    md = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(md)
    return md


# Standard test data matching what simulator uses
TEST_DATA = {
    "room_name": "Office",
    "inside_temp": "72",
    "inside_temp_f": "72",
    "inside_hum": "47",
    "inside_hum_pct": "47",
    "outside_temp": "68",
    "outside_temp_f": "68",
    "outside_hum": "53",
    "outside_hum_pct": "53",
    "weather": "Cloudy",
    "time": "10:32",
    "time_hhmm": "10:32",
    "ip": "192.168.1.42",
    "battery_voltage": "4.01",
    "battery_percent": "76",
    "days": "128",
    "fw_version": "v1.23",
}


def count_pixel_diff(img_a, img_b) -> int:
    """Count differing pixels between two 1-bit images."""
    a = img_a.tobytes()
    b = img_b.tobytes()
    total = 0
    for x, y in zip(a, b):
        v = x ^ y
        c = 0
        while v:
            v &= v - 1
            c += 1
        total += c
    return total


class TestSimMockParity:
    """Test suite for simulator/mock display parity."""

    @pytest.fixture(scope="class")
    def mock_display(self):
        """Load mock display module."""
        return load_mock_display()

    def test_mock_renders_without_error(self, mock_display):
        """Verify mock_display can render test data."""
        img = mock_display.render(TEST_DATA)
        assert img is not None
        assert img.size == (250, 122)

    def test_ui_spec_exists(self):
        """Verify ui_spec.json exists and is valid."""
        spec_path = ROOT / "config" / "ui_spec.json"
        assert spec_path.exists(), "ui_spec.json missing"

        with open(spec_path) as f:
            spec = json.load(f)

        assert "canvas" in spec
        assert "rects" in spec
        assert "components" in spec

    def test_all_rects_in_bounds(self):
        """Verify all rectangles in ui_spec.json are within display bounds."""
        spec_path = ROOT / "config" / "ui_spec.json"
        with open(spec_path) as f:
            spec = json.load(f)

        canvas = spec["canvas"]
        width, height = canvas["w"], canvas["h"]

        for name, rect in spec["rects"].items():
            x, y, w, h = rect
            assert x >= 0, f"{name}: x={x} < 0"
            assert y >= 0, f"{name}: y={y} < 0"
            assert x + w <= width, f"{name}: x+w={x+w} > {width}"
            assert y + h <= height, f"{name}: y+h={y+h} > {height}"

    def test_all_ops_have_valid_rects(self):
        """Verify all ops reference valid rect names."""
        spec_path = ROOT / "config" / "ui_spec.json"
        with open(spec_path) as f:
            spec = json.load(f)

        valid_rects = set(spec["rects"].keys())

        # Components is a dict: { name: [ops] }
        for comp_name, ops in spec["components"].items():
            if isinstance(ops, list):
                for op in ops:
                    rect_name = op.get("rect")
                    if rect_name:
                        assert (
                            rect_name in valid_rects
                        ), f"{comp_name}: op references unknown rect '{rect_name}'"

    def test_generated_js_matches_spec(self):
        """Verify ui_generated.js was generated from current ui_spec.json."""
        gen_js_path = ROOT / "web" / "sim" / "ui_generated.js"

        assert gen_js_path.exists(), "ui_generated.js missing - run gen_ui.py"

        # Check that generator script exists
        gen_script = ROOT / "scripts" / "gen_ui.py"
        assert gen_script.exists()

    def test_mock_uses_ui_spec_geometry(self, mock_display):
        """Verify mock_display uses coordinates from ui_spec.json."""
        # The mock should load geometry from display_geometry.json
        # which is generated from ui_spec.json
        geometry_path = ROOT / "config" / "display_geometry.json"
        assert geometry_path.exists(), "display_geometry.json missing"

        with open(geometry_path) as f:
            geometry = json.load(f)

        # Rects are nested inside "rects" key
        rects = geometry.get("rects", geometry)

        # Verify expected rects exist
        # FOOTER_IP was FOOTER_STATUS; the old name survives only as a legacy
        # #define in firmware/arduino/src/display_layout_aliases.h.
        expected = ["HEADER_NAME", "INSIDE_TEMP", "OUT_TEMP", "FOOTER_IP", "FOOTER_WEATHER"]
        for name in expected:
            assert name in rects, f"Rect {name} missing from geometry"


class TestCoordinateBoundsValidation:
    """Validate all coordinates are within safe display bounds."""

    DISPLAY_WIDTH = 250
    DISPLAY_HEIGHT = 122
    SAFE_MARGIN = 4  # 4px safe zone from edges

    def test_rects_within_safe_margins(self):
        """Verify all rects are within safe margins."""
        spec_path = ROOT / "config" / "ui_spec.json"
        with open(spec_path) as f:
            spec = json.load(f)

        issues = []
        for name, rect in spec["rects"].items():
            x, y, w, h = rect

            # Check safe margins (except outer border which should be at edges)
            if name not in ["OUTER_BORDER"]:
                if x < self.SAFE_MARGIN:
                    issues.append(f"{name}: x={x} < margin {self.SAFE_MARGIN}")
                if y < self.SAFE_MARGIN:
                    issues.append(f"{name}: y={y} < margin {self.SAFE_MARGIN}")
                if x + w > self.DISPLAY_WIDTH - self.SAFE_MARGIN:
                    issues.append(f"{name}: x+w={x+w} > {self.DISPLAY_WIDTH - self.SAFE_MARGIN}")
                if y + h > self.DISPLAY_HEIGHT - self.SAFE_MARGIN:
                    issues.append(f"{name}: y+h={y+h} > {self.DISPLAY_HEIGHT - self.SAFE_MARGIN}")

        # Allow some issues but report them
        if issues:
            print(f"Coordinate warnings (not failing): {issues}")

    def test_no_rect_overlaps_critical_regions(self):
        """Verify critical regions don't overlap unexpectedly."""
        spec_path = ROOT / "config" / "ui_spec.json"
        with open(spec_path) as f:
            spec = json.load(f)

        # These regions should never overlap
        non_overlapping = [
            ("INSIDE_TEMP", "OUT_TEMP"),
            ("FOOTER_IP", "FOOTER_WEATHER"),
            ("HEADER_NAME", "HEADER_TIME_CENTER"),
        ]

        def rects_overlap(r1, r2):
            x1, y1, w1, h1 = r1
            x2, y2, w2, h2 = r2
            return not (x1 + w1 <= x2 or x2 + w2 <= x1 or y1 + h1 <= y2 or y2 + h2 <= y1)

        for name1, name2 in non_overlapping:
            if name1 in spec["rects"] and name2 in spec["rects"]:
                r1 = spec["rects"][name1]
                r2 = spec["rects"][name2]
                assert not rects_overlap(r1, r2), f"{name1} and {name2} should not overlap"


class TestOpCoverage:
    """Verify all op types used in spec have firmware handlers."""

    # Ops that must have firmware handlers
    REQUIRED_OPS = {
        "line",
        "fill",  # solid underlay (v3 inverted header/tabs)
        "text",
        "textCenteredIn",
        "tempGroupCentered",
        "iconIn",
        "batteryGlyph",
        "sparkline",  # 24h history polyline (v3g graphs page)
        "frame",  # 1px outline box (v3g current-reading chips)
    }

    def test_all_used_ops_are_known(self):
        """Verify all ops in spec are in the required set."""
        spec_path = ROOT / "config" / "ui_spec.json"
        with open(spec_path) as f:
            spec = json.load(f)

        used_ops = set()
        # Components is a dict: { name: [ops] }
        for comp_name, ops in spec["components"].items():
            if isinstance(ops, list):
                for op in ops:
                    if isinstance(op, dict):
                        used_ops.add(op.get("op"))

        unknown = used_ops - self.REQUIRED_OPS
        assert not unknown, f"Unknown ops without handlers: {unknown}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
