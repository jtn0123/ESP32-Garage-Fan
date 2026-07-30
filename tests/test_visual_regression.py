#!/usr/bin/env python3
"""
Visual regression testing for ESP32 display simulator
Tests icons, battery states, text rendering, and UI element positioning
"""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional

import pytest

# Drives a real browser, so it belongs to the CI job that installs Chromium.
pytestmark = pytest.mark.slow

# Add project root to path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

try:
    import io

    import numpy as np
    from PIL import Image, ImageChops
    from playwright.sync_api import Page, sync_playwright
except ModuleNotFoundError as e:
    # Skip (not sys.exit) so an absent optional dep doesn't abort the whole
    # pytest run with an INTERNALERROR during collection. Only missing modules
    # skip; other ImportErrors from installed deps still fail loudly.
    pytest.skip(
        f"Missing dependency: {e.name} (install with: pip install numpy pillow playwright)",
        allow_module_level=True,
    )


@dataclass
class VisualTestCase:
    """A visual test case with expected results"""

    name: str
    category: str
    data: Dict[str, Any]
    expected_hash: Optional[str] = None
    max_diff_percentage: float = 0.5  # Maximum allowed pixel difference


class VisualRegressionTester:
    """Visual regression testing for the display simulator"""

    def __init__(self):
        self.web_root = ROOT / "web" / "sim"
        self.baseline_dir = ROOT / "tests" / "visual_baselines"
        self.output_dir = ROOT / "tests" / "visual_output"
        self.baseline_dir.mkdir(exist_ok=True)
        self.output_dir.mkdir(exist_ok=True)

    def _start_server(self, port: int) -> subprocess.Popen:
        """Start HTTP server for web simulator"""
        return subprocess.Popen(
            [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"],
            cwd=str(self.web_root),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def _find_free_port(self) -> int:
        """Find an available port"""
        import socket

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def capture_screenshot(self, page: Page, test_data: Dict[str, Any]) -> np.ndarray:
        """Capture screenshot with given test data"""
        # Apply test data
        page.evaluate(f"""
            window.testData = {json.dumps(test_data)};
            if (window.draw) window.draw(window.testData);
        """)

        # Wait for render
        page.wait_for_timeout(200)

        # Capture screenshot
        screenshot = page.screenshot(clip={"x": 0, "y": 0, "width": 250, "height": 122})
        img = Image.open(io.BytesIO(screenshot))
        return np.array(img)

    def compute_hash(self, image: np.ndarray) -> str:
        """Compute perceptual hash of image for comparison"""
        # Convert to grayscale
        if len(image.shape) == 3:
            gray = np.dot(image[..., :3], [0.2989, 0.5870, 0.1140])
        else:
            gray = image

        # Resize to 8x8 for DCT
        from PIL import Image

        img = Image.fromarray(gray.astype(np.uint8))
        img = img.resize((8, 8), Image.Resampling.LANCZOS)

        # Compute hash
        pixels = np.array(img).flatten()
        avg = pixels.mean()
        hash_bits = (pixels > avg).astype(int)

        # Convert to hex string
        hash_str = "".join(str(b) for b in hash_bits)
        return hashlib.md5(hash_str.encode()).hexdigest()[:16]

    def compare_images(self, img1: np.ndarray, img2: np.ndarray) -> float:
        """Compare two images and return difference percentage"""
        if img1.shape != img2.shape:
            return 100.0

        # Calculate pixel-wise difference
        diff = np.abs(img1.astype(float) - img2.astype(float))

        # Count significantly different pixels (threshold > 10)
        different_pixels = np.sum(diff > 10)
        total_pixels = img1.shape[0] * img1.shape[1]

        return (different_pixels / total_pixels) * 100

    def save_comparison(
        self, name: str, baseline: np.ndarray, current: np.ndarray, diff_pct: float
    ):
        """Save comparison images for debugging"""
        comparison = Image.new("RGB", (250 * 3 + 20, 122))

        # Baseline
        comparison.paste(Image.fromarray(baseline), (0, 0))

        # Current
        comparison.paste(Image.fromarray(current), (260, 0))

        # Difference
        baseline_img = Image.fromarray(baseline)
        current_img = Image.fromarray(current)
        diff_img = ImageChops.difference(baseline_img.convert("RGB"), current_img.convert("RGB"))
        comparison.paste(diff_img, (520, 0))

        # Save with metadata
        output_path = self.output_dir / f"{name}_comparison.png"
        comparison.save(output_path)

        # Also save metadata
        metadata = {"name": name, "diff_percentage": diff_pct, "timestamp": time.time()}
        (self.output_dir / f"{name}_metadata.json").write_text(json.dumps(metadata, indent=2))

    def run_test_suite(self, test_cases: List[VisualTestCase]) -> Dict[str, Any]:
        """Run visual regression test suite"""
        port = self._find_free_port()
        server = self._start_server(port)
        results = {"passed": [], "failed": [], "new": [], "total": len(test_cases)}

        try:
            time.sleep(1)  # Let server start

            with sync_playwright() as p:
                browser = p.chromium.launch()
                page = browser.new_page(viewport={"width": 700, "height": 400})
                page.goto(f"http://127.0.0.1:{port}/index.html", wait_until="load")
                page.wait_for_timeout(500)

                for test_case in test_cases:
                    # Capture current screenshot
                    current = self.capture_screenshot(page, test_case.data)
                    self.compute_hash(current)

                    # Check for baseline
                    baseline_path = self.baseline_dir / f"{test_case.name}.npy"

                    if baseline_path.exists():
                        # Load baseline and compare
                        baseline = np.load(baseline_path)
                        diff_pct = self.compare_images(baseline, current)

                        if diff_pct <= test_case.max_diff_percentage:
                            results["passed"].append(test_case.name)
                        else:
                            results["failed"].append(
                                {
                                    "name": test_case.name,
                                    "diff": diff_pct,
                                    "threshold": test_case.max_diff_percentage,
                                }
                            )
                            self.save_comparison(test_case.name, baseline, current, diff_pct)
                    else:
                        # New test, save baseline
                        np.save(baseline_path, current)
                        results["new"].append(test_case.name)

                browser.close()

        finally:
            server.terminate()
            server.wait(timeout=2)

        return results


# Define test cases
def get_weather_icon_tests() -> List[VisualTestCase]:
    """Test cases for weather icons"""
    return [
        VisualTestCase("icon_sunny", "weather", {"weather": "sunny", "outside_temp_f": "75"}),
        VisualTestCase(
            "icon_partly_cloudy", "weather", {"weather": "partly-cloudy", "outside_temp_f": "68"}
        ),
        VisualTestCase("icon_cloudy", "weather", {"weather": "cloudy", "outside_temp_f": "62"}),
        VisualTestCase("icon_rain", "weather", {"weather": "rain", "outside_temp_f": "55"}),
        VisualTestCase(
            "icon_pouring", "weather", {"weather": "pouring rain", "outside_temp_f": "50"}
        ),
        VisualTestCase(
            "icon_thunderstorm", "weather", {"weather": "thunderstorm", "outside_temp_f": "65"}
        ),
        VisualTestCase("icon_snow", "weather", {"weather": "snow", "outside_temp_f": "28"}),
        VisualTestCase("icon_fog", "weather", {"weather": "fog", "outside_temp_f": "45"}),
        VisualTestCase("icon_windy", "weather", {"weather": "windy", "wind_mph": "25"}),
        VisualTestCase("icon_night", "weather", {"weather": "clear-night", "outside_temp_f": "60"}),
        VisualTestCase("icon_hail", "weather", {"weather": "hail", "outside_temp_f": "35"}),
    ]


def get_battery_state_tests() -> List[VisualTestCase]:
    """Test cases for battery states"""
    return [
        VisualTestCase(
            "battery_critical", "battery", {"battery_percent": 5, "battery_voltage": 3.3}
        ),
        VisualTestCase("battery_low", "battery", {"battery_percent": 15, "battery_voltage": 3.5}),
        VisualTestCase(
            "battery_medium", "battery", {"battery_percent": 50, "battery_voltage": 3.7}
        ),
        VisualTestCase("battery_high", "battery", {"battery_percent": 85, "battery_voltage": 4.0}),
        VisualTestCase("battery_full", "battery", {"battery_percent": 100, "battery_voltage": 4.2}),
        VisualTestCase("battery_empty", "battery", {"battery_percent": 0, "battery_voltage": 3.0}),
    ]


def get_text_rendering_tests() -> List[VisualTestCase]:
    """Test cases for text rendering edge cases"""
    return [
        VisualTestCase(
            "text_overflow_room",
            "text",
            {"room_name": "Very Long Conference Room Name That Should Be Truncated"},
        ),
        VisualTestCase("text_unicode", "text", {"room_name": "Café École 日本 🏠"}),
        VisualTestCase("text_special_chars", "text", {"room_name": "Lab #3 & Test @ 50%"}),
        VisualTestCase(
            "text_max_temp", "text", {"inside_temp_f": "999.9", "outside_temp_f": "-99.9"}
        ),
        VisualTestCase("text_max_pressure", "text", {"pressure_hpa": "9999.9"}),
        VisualTestCase(
            "text_long_weather", "text", {"weather": "Heavy thunderstorms with torrential rain"}
        ),
        VisualTestCase(
            "text_all_fields",
            "text",
            {
                "room_name": "Test Room",
                "time_hhmm": "23:59",
                "inside_temp_f": "72.5",
                "inside_hum_pct": "100",
                "pressure_hpa": "1013",
                "co2_ppm": "9999",
                "outside_temp_f": "68.4",
                "outside_hum_pct": "100",
                "wind_mph": "99.9",
                "weather": "storm",
                "battery_percent": "100",
                "ip": "192.168.1.100",
            },
        ),
    ]


def get_missing_data_tests() -> List[VisualTestCase]:
    """Test cases for missing data scenarios"""
    return [
        VisualTestCase("missing_inside", "missing", {"inside_temp_f": "", "inside_hum_pct": ""}),
        VisualTestCase(
            "missing_outside",
            "missing",
            {"outside_temp_f": "", "outside_hum_pct": "", "weather": ""},
        ),
        VisualTestCase(
            "missing_all",
            "missing",
            {
                "inside_temp_f": "",
                "outside_temp_f": "",
                "inside_hum_pct": "",
                "outside_hum_pct": "",
                "weather": "",
                "ip": "",
            },
        ),
        VisualTestCase(
            "missing_partial",
            "missing",
            {"inside_temp_f": "72", "inside_hum_pct": "", "pressure_hpa": ""},
        ),
    ]


def get_alignment_tests() -> List[VisualTestCase]:
    """Test cases for UI alignment"""
    return [
        VisualTestCase("align_baseline", "alignment", {}),  # Default layout
        VisualTestCase("align_minimal", "alignment", {"room_name": "A", "time_hhmm": "1:00"}),
        VisualTestCase(
            "align_maximal",
            "alignment",
            {
                "room_name": "Maximum Length Room Name Here",
                "time_hhmm": "23:59",
                "inside_temp_f": "999.9",
                "outside_temp_f": "999.9",
            },
        ),
    ]


# Pytest tests
@pytest.fixture
def tester():
    """Create visual regression tester instance"""
    return VisualRegressionTester()


def test_weather_icons(tester):
    """Test weather icon rendering"""
    test_cases = get_weather_icon_tests()
    results = tester.run_test_suite(test_cases)

    # Report results
    if results["failed"]:
        failures = "\n".join(
            [f"  - {f['name']}: {f['diff']:.2f}% > {f['threshold']}%" for f in results["failed"]]
        )
        pytest.fail(f"Visual regression failures:\n{failures}")

    # New baselines are okay
    if results["new"]:
        print(f"Created {len(results['new'])} new baselines")


def test_battery_states(tester):
    """Test battery indicator states"""
    test_cases = get_battery_state_tests()
    results = tester.run_test_suite(test_cases)

    if results["failed"]:
        failures = "\n".join([f"  - {f['name']}: {f['diff']:.2f}%" for f in results["failed"]])
        pytest.fail(f"Battery display regression:\n{failures}")


def test_text_rendering(tester):
    """Test text rendering edge cases"""
    test_cases = get_text_rendering_tests()
    results = tester.run_test_suite(test_cases)

    # Text rendering may have slightly higher variance
    for test in test_cases:
        test.max_diff_percentage = 1.0  # Allow 1% difference for anti-aliasing

    if results["failed"]:
        failures = "\n".join([f"  - {f['name']}: {f['diff']:.2f}%" for f in results["failed"]])
        pytest.fail(f"Text rendering regression:\n{failures}")


@pytest.mark.skip(reason="Covered by test_comprehensive_suite")
def test_missing_data_display(tester):
    """Test display with missing data"""
    test_cases = get_missing_data_tests()
    results = tester.run_test_suite(test_cases)

    if results["failed"]:
        failures = "\n".join([f"  - {f['name']}: {f['diff']:.2f}%" for f in results["failed"]])
        pytest.fail(f"Missing data display regression:\n{failures}")


@pytest.mark.skip(reason="Covered by test_comprehensive_suite")
def test_ui_alignment(tester):
    """Test UI element alignment"""
    test_cases = get_alignment_tests()

    # Alignment should be pixel-perfect - set before running tests
    for test in test_cases:
        test.max_diff_percentage = 0.1  # Very strict

    results = tester.run_test_suite(test_cases)

    if results["failed"]:
        failures = "\n".join([f"  - {f['name']}: {f['diff']:.2f}%" for f in results["failed"]])
        pytest.fail(f"UI alignment regression:\n{failures}")


def test_comprehensive_suite(tester):
    """Run comprehensive visual regression suite"""
    all_tests = (
        get_weather_icon_tests()
        + get_battery_state_tests()
        + get_text_rendering_tests()
        + get_missing_data_tests()
        + get_alignment_tests()
    )

    results = tester.run_test_suite(all_tests)

    # Generate report
    report = {
        "total": results["total"],
        "passed": len(results["passed"]),
        "failed": len(results["failed"]),
        "new": len(results["new"]),
        "pass_rate": (
            (len(results["passed"]) / results["total"] * 100) if results["total"] > 0 else 0
        ),
    }

    print("\nVisual Regression Report:")
    print(f"  Total tests: {report['total']}")
    print(f"  Passed: {report['passed']}")
    print(f"  Failed: {report['failed']}")
    print(f"  New baselines: {report['new']}")
    print(f"  Pass rate: {report['pass_rate']:.1f}%")

    # Fail if regression detected
    if results["failed"]:
        failures = "\n".join(
            [f"  - {f['name']}: {f['diff']:.2f}% > {f['threshold']}%" for f in results["failed"]]
        )
        pytest.fail(f"{len(results['failed'])} visual regressions detected:\n{failures}")


if __name__ == "__main__":
    # Run tests
    pytest.main([__file__, "-v"])
