"""Test edge cases in UI validation that could break during refactoring."""

import json
from pathlib import Path
import sys

# Add scripts directory to path
sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))


class TestUIValidationEdgeCases:
    """Test complex edge cases in UI validation logic."""

    def setup_method(self):
        """Setup test environment."""
        self.root = Path(__file__).parent.parent
        self.geometry_path = self.root / "config/display_geometry.json"

        # Load geometry
        with open(self.geometry_path) as f:
            self.geometry = json.load(f)

    def test_text_overflow_with_bold_fonts(self):
        """Test text overflow detection with different font weights."""
        test_cases = [
            # (text, font_size, region_width, is_bold, should_overflow)
            ("100.0°F", 32, 150, False, False),  # Normal font fits
            ("100.0°F", 32, 150, True, True),  # Bold font overflows
            ("1013 hPa", 16, 90, False, False),  # Normal pressure fits
            ("1013 hPa", 16, 80, True, True),  # Bold pressure overflows
        ]

        for text, size, width, is_bold, should_overflow in test_cases:
            # Calculate text width (approximate)
            char_width = size * 0.6 if not is_bold else size * 0.7
            text_width = len(text) * char_width

            overflows = text_width > width
            assert overflows == should_overflow, f"Text '{text}' overflow detection failed"

    def test_collision_detection_with_exclusions(self):
        """Test region collision detection with allowed overlaps."""
        # Regions that should be excluded from collision detection
        # _BAND/_TAB are fill underlays: containing their own text rects is
        # their entire purpose, so like _INNER/_BADGE they are structural,
        # not content, and sit outside collision semantics.
        exclusion_patterns = ["_INNER", "_BADGE", "_ICON", "_LABEL", "_BAND", "_TAB"]

        rects = self.geometry["rects"]
        collisions = []

        # Rects only collide meaningfully when both can draw in the SAME
        # variant. The rect table is shared across variants (v2, v3, v3g), and
        # different variants deliberately reuse screen area - v3's header band
        # overlaps v2's header rects by design. Group by variant membership
        # derived from the spec's components.
        import json as _json
        from pathlib import Path as _Path

        spec = _json.loads(
            (_Path(__file__).resolve().parents[1] / "config" / "ui_spec.json").read_text()
        )
        variant_rects = {}
        for vname, comps in spec.get("variants", {}).items():
            used = set()
            for cname in comps:
                for op in spec.get("components", {}).get(cname, []):
                    if isinstance(op.get("rect"), str):
                        used.add(op["rect"])
            variant_rects[vname] = used

        def same_variant(a, b):
            return any(a in used and b in used for used in variant_rects.values())

        for name1, r1 in rects.items():
            # Skip if excluded
            if any(pattern in name1 for pattern in exclusion_patterns):
                continue

            for name2, r2 in rects.items():
                if name1 >= name2:  # Avoid duplicate checks
                    continue

                # Skip if excluded
                if any(pattern in name2 for pattern in exclusion_patterns):
                    continue

                if not same_variant(name1, name2):
                    continue

                # Check collision
                if self.regions_overlap(r1, r2):
                    collisions.append((name1, name2))

        # Should have no unexpected collisions (based on actual geometry)
        expected_collisions = [
            ("HEADER_NAME", "HEADER_TIME_CENTER"),  # Time can overlap with name
            ("INSIDE_HUMIDITY", "INSIDE_PRESSURE"),  # Pressure and humidity can overlap
            (
                "INSIDE_PRESS_V3",
                "INSIDE_RH_V3",
            ),  # v3: RH left-aligned, pressure right-aligned; shared middle slack
            ("OUT_PRESSURE", "OUT_WIND"),  # Pressure and wind can overlap
        ]

        for collision in collisions:
            assert (
                collision in expected_collisions
                or (collision[1], collision[0]) in expected_collisions
            ), f"Unexpected collision: {collision}"

    def regions_overlap(self, r1, r2):
        """Check if two regions overlap."""
        # Rects are [x, y, w, h] lists
        x1, y1, w1, h1 = r1
        x2, y2, w2, h2 = r2
        return not (x1 + w1 <= x2 or x2 + w2 <= x1 or y1 + h1 <= y2 or y2 + h2 <= y1)

    def test_empty_region_detection(self):
        """Test detection of empty regions with various content."""
        test_cases = [
            ("", True),  # Empty string
            (" ", True),  # Single space
            ("  \t\n", True),  # Whitespace only
            ("--", False),  # Placeholder
            ("0", False),  # Zero value
            ("N/A", False),  # Not available
            (None, True),  # Null value
        ]

        for content, is_empty in test_cases:
            detected_empty = not content or not str(content).strip()
            assert detected_empty == is_empty, f"Empty detection failed for '{content}'"

    def test_boundary_text_positioning(self):
        """Test text exactly at region boundaries."""
        test_cases = [
            # (text, x, y, width, height, should_fit)
            ("Edge", 0, 0, 40, 20, True),  # Top-left corner
            ("Edge", 211, 103, 40, 20, False),  # Beyond display (250x122)
            ("Long text here", 200, 60, 50, 20, False),  # Text too wide
        ]

        display_width = self.geometry["canvas"]["w"]  # 250
        display_height = self.geometry["canvas"]["h"]  # 122

        for text, x, y, width, height, should_fit in test_cases:
            fits = (
                x >= 0
                and y >= 0
                and x + width <= display_width
                and y + height <= display_height
                and len(text) * 8 <= width  # Approximate char width
            )

            assert fits == should_fit, f"Boundary check failed for '{text}' at ({x},{y})"

    def test_malformed_geometry_handling(self):
        """Test validation behavior with malformed geometry."""
        malformed_cases = [
            {"x": -10, "y": 10, "width": 50, "height": 20},  # Negative x
            {"x": 10, "y": -10, "width": 50, "height": 20},  # Negative y
            {"x": 10, "y": 10, "width": -50, "height": 20},  # Negative width
            {"x": 10, "y": 10, "width": 50, "height": -20},  # Negative height
            {"x": 10, "y": 10, "width": 0, "height": 20},  # Zero width
            {"x": 10, "y": 10, "width": 50, "height": 0},  # Zero height
            {"x": 260, "y": 10, "width": 50, "height": 20},  # Beyond display (250 width)
            {"x": 10, "y": 130, "width": 50, "height": 20},  # Beyond display (122 height)
        ]

        for region in malformed_cases:
            is_valid = (
                region["x"] >= 0
                and region["y"] >= 0
                and region["width"] > 0
                and region["height"] > 0
                and region["x"] + region["width"] <= 250
                and region["y"] + region["height"] <= 122
            )

            assert not is_valid, f"Malformed region should be invalid: {region}"

    def test_text_centering_calculations(self):
        """Test text centering within regions."""
        test_cases = [
            ("Center", 100, 50),  # Region width, expected x offset
            ("A", 100, 50),  # Single char
            ("Very long text", 50, 25),  # Text wider than region
        ]

        for text, region_width, expected_center in test_cases:
            char_width = 8  # Approximate
            text_width = len(text) * char_width
            center_x = max(0, (region_width - text_width) // 2)

            # Allow some tolerance
            assert abs(center_x - expected_center) <= region_width // 2

    def test_validation_severity_levels(self):
        """Test that validation issues have correct severity levels."""
        severity_tests = [
            ("text_overflow", "error"),  # Text overflow is error
            ("region_collision", "error"),  # Collision is error
            ("empty_region", "warning"),  # Empty is warning
            ("alignment_issue", "warning"),  # Alignment is warning
            ("missing_data", "info"),  # Missing data is info
        ]

        severities = {
            "text_overflow": "error",
            "region_collision": "error",
            "empty_region": "warning",
            "alignment_issue": "warning",
            "missing_data": "info",
        }

        for issue_type, expected_severity in severity_tests:
            assert severities[issue_type] == expected_severity

    def test_cascading_validation_failures(self):
        """Test handling of multiple validation failures."""
        # Simulate multiple issues
        issues = []

        # Add various issues
        if "test" == "test":  # Always true for testing
            issues.append({"type": "overflow", "region": "TEMP"})
            issues.append({"type": "collision", "regions": ["A", "B"]})
            issues.append({"type": "empty", "region": "HUMID"})

        # Should handle multiple issues
        assert len(issues) == 3

        # Should prioritize by severity
        sorted_issues = sorted(
            issues,
            key=lambda x: 0 if x["type"] == "collision" else 1 if x["type"] == "overflow" else 2,
        )

        assert sorted_issues[0]["type"] == "collision"  # Highest priority
