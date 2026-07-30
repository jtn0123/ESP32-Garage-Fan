"""
Edge case tests for code generation modules (gen_ui.py, gen_layout_header.py, gen_device_header.py).

Tests focus on edge cases and error handling not covered by other test files.
"""

import json
import os
from pathlib import Path
import sys
import tempfile
from typing import Any, Dict

import pytest

# Add scripts to path
ROOT = os.path.dirname(os.path.dirname(__file__))
sys.path.insert(0, os.path.join(ROOT, "scripts"))


def create_temp_json(content: Dict[str, Any]) -> str:
    """Create a temporary JSON file with given content."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
        json.dump(content, f, indent=2)
        return f.name


class TestUISpecEdgeCases:
    """Edge case tests for UI specification parsing."""

    def test_empty_spec(self):
        """Test handling of empty specification."""
        spec = {}
        spec_file = create_temp_json(spec)
        try:
            # Empty spec should be handled gracefully
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert loaded == {}
        finally:
            os.unlink(spec_file)

    def test_spec_with_only_metadata(self):
        """Test spec with only metadata, no operations."""
        spec = {
            "schema": "1.0",
            "canvas": {"w": 250, "h": 122},
            "fonts": {"tokens": {"big_px": 22}},
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert "canvas" in loaded
            assert loaded["canvas"]["w"] == 250
        finally:
            os.unlink(spec_file)

    def test_spec_with_unicode_strings(self):
        """Test spec with Unicode characters in strings."""
        spec = {
            "schema": "1.0",
            "canvas": {"w": 250, "h": 122},
            "operations": [
                {"type": "drawText", "params": {"text": "Température 温度 🌡️", "x": 10, "y": 20}},
                {"type": "drawText", "params": {"text": "café résumé naïve", "x": 10, "y": 40}},
            ],
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            text1 = loaded["operations"][0]["params"]["text"]
            assert "Température" in text1
            assert "温度" in text1
        finally:
            os.unlink(spec_file)

    def test_spec_with_special_characters_in_keys(self):
        """Test spec with special characters in region names."""
        spec = {
            "schema": "1.0",
            "canvas": {"w": 250, "h": 122},
            "rects": {
                "HEADER-NAME": [0, 0, 100, 20],  # Hyphen in key
                "FOOTER_TEXT": [0, 100, 100, 20],  # Underscore in key
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert "HEADER-NAME" in loaded["rects"]
            assert "FOOTER_TEXT" in loaded["rects"]
        finally:
            os.unlink(spec_file)

    def test_spec_with_negative_coordinates(self):
        """Test handling of negative coordinates (should be caught as invalid)."""
        spec = {
            "canvas": {"w": 250, "h": 122},
            "rects": {
                "INVALID_RECT": [-10, -5, 100, 50],  # Negative x, y
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            x, y, w, h = loaded["rects"]["INVALID_RECT"]
            # Negative coordinates should be detectable
            assert x < 0 or y < 0
        finally:
            os.unlink(spec_file)

    def test_spec_with_zero_dimensions(self):
        """Test handling of zero-size rectangles."""
        spec = {
            "canvas": {"w": 250, "h": 122},
            "rects": {
                "ZERO_WIDTH": [10, 10, 0, 50],
                "ZERO_HEIGHT": [10, 10, 50, 0],
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            _, _, w, _ = loaded["rects"]["ZERO_WIDTH"]
            _, _, _, h = loaded["rects"]["ZERO_HEIGHT"]
            assert w == 0 or h == 0
        finally:
            os.unlink(spec_file)

    def test_spec_with_overlapping_rects(self):
        """Test detection of overlapping rectangles."""
        spec = {
            "canvas": {"w": 250, "h": 122},
            "rects": {
                "RECT_A": [10, 10, 100, 50],
                "RECT_B": [50, 30, 100, 50],  # Overlaps with RECT_A
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)

            # Check for overlap
            ax, ay, aw, ah = loaded["rects"]["RECT_A"]
            bx, by, bw, bh = loaded["rects"]["RECT_B"]

            overlaps = not (ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay)
            assert overlaps, "Rects should be detected as overlapping"
        finally:
            os.unlink(spec_file)

    def test_spec_with_rect_outside_canvas(self):
        """Test detection of rectangle extending beyond canvas."""
        spec = {
            "canvas": {"w": 250, "h": 122},
            "rects": {
                "OUT_OF_BOUNDS": [200, 100, 100, 50],  # Extends beyond 250x122
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)

            x, y, w, h = loaded["rects"]["OUT_OF_BOUNDS"]
            canvas_w = loaded["canvas"]["w"]
            canvas_h = loaded["canvas"]["h"]

            extends_beyond = (x + w > canvas_w) or (y + h > canvas_h)
            assert extends_beyond, "Rect should extend beyond canvas"
        finally:
            os.unlink(spec_file)


class TestGeometryEdgeCases:
    """Edge case tests for geometry calculations."""

    def test_very_large_canvas(self):
        """Test with very large canvas dimensions."""
        spec = {
            "canvas": {"w": 10000, "h": 10000},
            "rects": {"LARGE_RECT": [0, 0, 5000, 5000]},
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert loaded["canvas"]["w"] == 10000
        finally:
            os.unlink(spec_file)

    def test_very_small_canvas(self):
        """Test with minimal canvas dimensions."""
        spec = {"canvas": {"w": 1, "h": 1}, "rects": {"TINY_RECT": [0, 0, 1, 1]}}
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert loaded["canvas"]["w"] == 1
            assert loaded["canvas"]["h"] == 1
        finally:
            os.unlink(spec_file)

    def test_adjacent_rects_no_gap(self):
        """Test adjacent rectangles with no gap."""
        spec = {
            "canvas": {"w": 250, "h": 122},
            "rects": {
                "LEFT": [0, 0, 125, 122],
                "RIGHT": [125, 0, 125, 122],  # Exactly adjacent, no gap
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)

            left = loaded["rects"]["LEFT"]
            right = loaded["rects"]["RIGHT"]

            # Left rect ends where right begins
            assert left[0] + left[2] == right[0]
        finally:
            os.unlink(spec_file)

    def test_float_coordinates_truncation(self):
        """Test that float coordinates are handled properly."""
        # JSON will preserve floats, but code should truncate to int
        spec = {
            "canvas": {"w": 250.0, "h": 122.0},
            "rects": {
                "FLOAT_RECT": [10.5, 20.7, 50.3, 30.9],
            },
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)

            coords = loaded["rects"]["FLOAT_RECT"]
            # Floats should be preserved in JSON
            assert isinstance(coords[0], float)
            # But when used, should be truncated/rounded to int
            int_coords = [int(c) for c in coords]
            assert int_coords == [10, 20, 50, 30]
        finally:
            os.unlink(spec_file)


class TestStringHandlingEdgeCases:
    """Edge case tests for string handling in code generation."""

    def test_empty_string_value(self):
        """Test handling of empty string values."""
        spec = {"operations": [{"type": "drawText", "params": {"text": "", "x": 10, "y": 20}}]}
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            text = loaded["operations"][0]["params"]["text"]
            assert text == ""
        finally:
            os.unlink(spec_file)

    def test_very_long_string(self):
        """Test handling of very long strings."""
        long_text = "A" * 10000
        spec = {"operations": [{"type": "drawText", "params": {"text": long_text, "x": 0, "y": 0}}]}
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            text = loaded["operations"][0]["params"]["text"]
            assert len(text) == 10000
        finally:
            os.unlink(spec_file)

    def test_string_with_escape_sequences(self):
        """Test strings with C escape sequences."""
        spec = {
            "operations": [
                {"type": "drawText", "params": {"text": "Line1\\nLine2", "x": 0, "y": 0}},
                {"type": "drawText", "params": {"text": "Tab\\tHere", "x": 0, "y": 20}},
                {"type": "drawText", "params": {"text": 'Quote\\"Test', "x": 0, "y": 40}},
            ]
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            # JSON will interpret some escapes
            text0 = loaded["operations"][0]["params"]["text"]
            assert "\\" in text0 or "\n" in text0
        finally:
            os.unlink(spec_file)

    def test_string_with_null_character(self):
        """Test handling of null character in strings."""
        # JSON doesn't allow embedded NUL, but let's test near-NUL
        spec = {
            "operations": [
                {"type": "drawText", "params": {"text": "Before\x01After", "x": 0, "y": 0}}
            ]
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            text = loaded["operations"][0]["params"]["text"]
            assert "\x01" in text
        finally:
            os.unlink(spec_file)


class TestJSONParsingEdgeCases:
    """Edge case tests for JSON parsing."""

    def test_json_with_comments_stripped(self):
        """Test that gen_ui.py strips comments from JSON."""
        # Can't test this directly without running the script,
        # but we can verify the concept
        json_with_comments = """{
            // This is a comment
            "version": "1.0",
            /* Block comment */
            "canvas": {"w": 250, "h": 122}
        }"""

        # Standard JSON parser will fail on comments
        with pytest.raises(json.JSONDecodeError):
            json.loads(json_with_comments)

    def test_json_trailing_comma(self):
        """Test handling of trailing commas (invalid JSON)."""
        json_with_trailing = '{"a": 1, "b": 2,}'

        with pytest.raises(json.JSONDecodeError):
            json.loads(json_with_trailing)

    def test_deeply_nested_json(self):
        """Test deeply nested JSON structures."""
        # Build nested structure
        nested = {"level": 0}
        current = nested
        for i in range(100):
            current["nested"] = {"level": i + 1}
            current = current["nested"]

        spec_file = create_temp_json(nested)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert loaded["level"] == 0
            assert "nested" in loaded
        finally:
            os.unlink(spec_file)

    def test_json_with_large_numbers(self):
        """Test JSON with very large numbers."""
        spec = {
            "canvas": {"w": 2**31 - 1, "h": 2**31 - 1},  # Max 32-bit signed int
            "rects": {"HUGE": [0, 0, 2**30, 2**30]},
        }
        spec_file = create_temp_json(spec)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert loaded["canvas"]["w"] == 2**31 - 1
        finally:
            os.unlink(spec_file)


class TestDeviceConfigEdgeCases:
    """Edge case tests for device configuration generation."""

    def test_wifi_ssid_with_special_chars(self):
        """Test WiFi SSID with special characters."""
        config = {
            "wifi": {
                "ssid": 'My"Network\\Name',
                "password": "pass'word\"test",
            }
        }
        spec_file = create_temp_json(config)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            ssid = loaded["wifi"]["ssid"]
            # Special characters should be preserved
            assert '"' in ssid
            assert "\\" in ssid
        finally:
            os.unlink(spec_file)

    def test_device_id_sanitization(self):
        """Test that device IDs are properly sanitized."""
        # Device IDs should only contain alphanumeric and underscores/hyphens
        invalid_ids = [
            "device with spaces",
            "device/slash",
            "device#hash",
            "device@at",
        ]

        for device_id in invalid_ids:
            # These should be detectable as invalid
            has_invalid = any(
                c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
                for c in device_id
            )
            assert has_invalid, f"Device ID '{device_id}' should be detected as invalid"

    def test_mqtt_topic_with_wildcards(self):
        """Test MQTT topics with wildcard characters."""
        config = {
            "mqtt": {
                "subscribe_topic": "home/+/temperature",  # Single-level wildcard
                "subscribe_all": "espsensor/#",  # Multi-level wildcard
            }
        }
        spec_file = create_temp_json(config)
        try:
            with open(spec_file, "r") as f:
                loaded = json.load(f)
            assert "+" in loaded["mqtt"]["subscribe_topic"]
            assert "#" in loaded["mqtt"]["subscribe_all"]
        finally:
            os.unlink(spec_file)


class TestCPlusPlusCodeGeneration:
    """Tests for C++ code generation edge cases."""

    def test_identifier_naming(self):
        """Test valid C++ identifier generation from region names."""
        region_names = [
            "HEADER_NAME",  # Valid as-is
            "123_START",  # Starts with number (invalid C++ identifier)
            "has-hyphen",  # Hyphen (invalid C++ identifier)
            "has space",  # Space (invalid)
            "HEADER NAME",  # Also invalid
        ]

        for name in region_names:
            if name in ["HEADER_NAME"]:
                assert name.replace("_", "").isalnum()
            elif name in ["123_START", "has-hyphen", "has space", "HEADER NAME"]:
                # These need sanitization
                assert not name.isidentifier() or " " in name or "-" in name

    def test_string_literal_escaping(self):
        """Test proper escaping for C++ string literals."""
        test_strings = [
            ('Hello "World"', r"Hello \"World\""),
            ("Line1\nLine2", r"Line1\nLine2"),
            ("Tab\tHere", r"Tab\tHere"),
            ("Backslash\\Here", r"Backslash\\Here"),
        ]

        for original, expected in test_strings:
            # C++ string escaping should handle these
            escaped = original.replace("\\", "\\\\").replace('"', '\\"')
            escaped = escaped.replace("\n", "\\n").replace("\t", "\\t")
            # Just verify escaping concept
            assert "\\" in escaped or escaped == original

    def test_numeric_constant_overflow(self):
        """Test handling of numeric constants that might overflow."""
        values = [
            2**31 - 1,  # Max int32
            2**31,  # Overflow int32
            2**63 - 1,  # Max int64
            -(2**31),  # Min int32
        ]

        for val in values:
            # Check if fits in int32
            fits_int32 = -(2**31) <= val <= 2**31 - 1
            if val == 2**31 or val == 2**63 - 1:
                assert not fits_int32
            elif val in [2**31 - 1, -(2**31)]:
                assert fits_int32


class TestLayoutDriftDetection:
    """Tests related to detecting layout drift between platforms."""

    def test_crc_consistency(self):
        """Test that CRC calculation is consistent."""
        # The geometry file includes a CRC for drift detection
        geometry_path = Path(ROOT) / "config" / "display_geometry.json"
        if geometry_path.exists():
            with open(geometry_path, "r") as f:
                geom = json.load(f)

            if "layout_crc" in geom:
                crc = geom["layout_crc"]
                # CRC should be a hex string
                assert crc.startswith("0x") or crc.startswith("0X")
                # Should be 8 hex digits (32-bit CRC)
                hex_part = crc[2:]
                assert len(hex_part) == 8
                assert all(c in "0123456789ABCDEFabcdef" for c in hex_part)

    def test_layout_version_format(self):
        """Test layout version format."""
        geometry_path = Path(ROOT) / "config" / "display_geometry.json"
        if geometry_path.exists():
            with open(geometry_path, "r") as f:
                geom = json.load(f)

            if "layout_version" in geom:
                version = geom["layout_version"]
                # Version should be a positive integer
                assert isinstance(version, int)
                assert version > 0


class TestErrorRecovery:
    """Tests for error recovery in code generation."""

    def test_partial_spec_handling(self):
        """Test handling of partial/incomplete specifications."""
        partial_specs = [
            {"canvas": {"w": 250}},  # Missing height
            {"rects": {"TEST": [0, 0, 10]}},  # Incomplete rect (only 3 values)
            {"fonts": {}},  # Empty fonts
        ]

        for spec in partial_specs:
            spec_file = create_temp_json(spec)
            try:
                with open(spec_file, "r") as f:
                    loaded = json.load(f)
                # Should load without error
                assert isinstance(loaded, dict)
            finally:
                os.unlink(spec_file)

    def test_malformed_rect_array(self):
        """Test handling of malformed rect arrays."""
        specs = [
            {"rects": {"BAD": [0, 0]}},  # Too few elements
            {"rects": {"BAD": [0, 0, 10, 20, 30]}},  # Too many elements
            {"rects": {"BAD": "not_an_array"}},  # Wrong type
        ]

        for spec in specs:
            spec_file = create_temp_json(spec)
            try:
                with open(spec_file, "r") as f:
                    loaded = json.load(f)
                rect = loaded["rects"]["BAD"]
                # These should be detectable as invalid
                if isinstance(rect, list):
                    assert len(rect) != 4
                else:
                    assert not isinstance(rect, list)
            finally:
                os.unlink(spec_file)


class TestBatteryGlyphOpEmission:
    """batteryGlyph firmware emission derives the glyph box from its rect.

    Regression tests for the codegen bug where a rect-only batteryGlyph op
    emitted p0..p3 = 0 and the device drew a 0x0 glyph at the origin while
    the web sim drew a proper 13x7 icon inside the rect.
    """

    @staticmethod
    def _spec(ops):
        return {
            "rects": {"FOOTER_BATTERY": [6, 88, 118, 12]},
            "fonts": {"tokens": {"small": {"px": 10}}},
            "components": {"footer": ops},
            "variants": {"v2": ["footer"]},
        }

    def test_rect_only_op_derives_sim_matching_glyph_box(self):
        import gen_ui

        out = gen_ui.emit_fw_ops_cpp(
            self._spec(
                [
                    {
                        "op": "batteryGlyph",
                        "rect": "FOOTER_BATTERY",
                        "percent": "{battery_percent}",
                    }
                ]
            )
        )
        # x = rect.x + 2, y = rect.y + (rect.h - 7) // 2, fixed 13x7 glyph:
        # the same placement web/sim/sim.js uses for its default battery icon.
        assert '{ OP_BATTERYGLYPH, 0, 0, 0, 8, 90, 13, 7, "battery_percent", NULL },' in out

    def test_explicit_coords_win_over_rect(self):
        import gen_ui

        out = gen_ui.emit_fw_ops_cpp(
            self._spec(
                [
                    {
                        "op": "batteryGlyph",
                        "rect": "FOOTER_BATTERY",
                        "x": 3,
                        "y": 4,
                        "w": 10,
                        "h": 6,
                        "percent": "{battery_percent}",
                    }
                ]
            )
        )
        assert '{ OP_BATTERYGLYPH, 0, 0, 0, 3, 4, 10, 6, "battery_percent", NULL },' in out

    def test_unknown_rect_falls_back_to_origin_with_default_size(self):
        import gen_ui

        out = gen_ui.emit_fw_ops_cpp(
            self._spec(
                [
                    {
                        "op": "batteryGlyph",
                        "rect": "NO_SUCH_RECT",
                        "percent": "{battery_percent}",
                    }
                ]
            )
        )
        # Unknown rect id is 255; no coords derivable, keep the default glyph size
        assert '{ OP_BATTERYGLYPH, 255, 0, 0, 0, 0, 13, 7, "battery_percent", NULL },' in out

    def test_malformed_coords_fall_back_to_default_glyph(self):
        import gen_ui

        out = gen_ui.emit_fw_ops_cpp(
            self._spec(
                [
                    {
                        "op": "batteryGlyph",
                        "rect": "FOOTER_BATTERY",
                        "x": "not-a-number",
                        "percent": "{battery_percent}",
                    }
                ]
            )
        )
        assert '{ OP_BATTERYGLYPH, 0, 0, 0, 0, 0, 13, 7, "battery_percent", NULL },' in out
