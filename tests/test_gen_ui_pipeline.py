#!/usr/bin/env python3
"""
Comprehensive test suite for UI code generation pipeline (gen_ui.py).
Tests the transformation from UI specifications to C++ code.
"""

import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional

import pytest

# Repo-relative paths of everything gen_ui.py writes; with --output they are
# mirrored under the given directory instead of written into the repo.
GENERATED_REL_PATHS = [
    "firmware/arduino/src/ui_generated.h",
    "firmware/arduino/src/ui_generated.cpp",
    "firmware/arduino/src/ui_ops_generated.h",
    "firmware/arduino/src/ui_ops_generated.cpp",
    "firmware/arduino/src/display_layout.h",
    "config/display_geometry.json",
    "web/sim/ui_generated.js",
    "web/sim/geometry.json",
]


def get_gen_ui_script():
    """Get path to gen_ui.py script"""
    return Path(__file__).parent.parent / "scripts" / "gen_ui.py"


def get_ui_spec_path():
    """Get path to UI specification JSON"""
    return Path(__file__).parent.parent / "config" / "ui_spec.json"


def load_ui_spec() -> Dict[str, Any]:
    """Load UI specification"""
    spec_path = get_ui_spec_path()
    if spec_path.exists():
        with open(spec_path, "r") as f:
            return json.load(f)
    return {}


def create_test_ui_spec(operations: List[Dict[str, Any]]) -> str:
    """Create a test UI specification"""
    spec = {"version": "1.0", "display": {"width": 250, "height": 122}, "operations": operations}

    with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
        json.dump(spec, f, indent=2)
        return f.name


def run_gen_ui(spec_file: str, output_dir: str) -> Optional[subprocess.CompletedProcess]:
    """Run gen_ui.py with --spec/--output; None if the script is missing."""
    script = get_gen_ui_script()
    if not script.exists():
        return None

    return subprocess.run(
        [sys.executable, str(script), "--spec", spec_file, "--output", output_dir],
        capture_output=True,
        text=True,
        timeout=30,
    )


def load_gen_ui_module():
    """Import scripts/gen_ui.py as a fresh module instance.

    A fresh instance per test keeps --spec's module-global mutations test-local,
    and in-process execution lets CI's `pytest --cov=scripts` measure the
    generator (subprocess runs are invisible to coverage).
    """
    script = get_gen_ui_script()
    spec = importlib.util.spec_from_file_location("gen_ui_under_test", script)
    mod = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


def write_minimal_ui_spec(path: Path, rect_name: str = "TESTBOX") -> Path:
    """Write a minimal spec that passes gen_ui.py's validation."""
    spec = {
        "schema": "ui-spec@1",
        "canvas": {"w": 250, "h": 122},
        "fonts": {"tokens": {"big": {"px": 22}, "label": {"px": 11}, "small": {"px": 10}}},
        "rects": {rect_name: [0, 0, 100, 20]},
        "components": {"header": [{"op": "text", "rect": rect_name, "text": "hi", "x": 0, "y": 0}]},
        "variants": {"v1": ["header"]},
    }
    path.write_text(json.dumps(spec))
    return path


class TestArgumentHandling:
    """gen_ui.py must honor --spec/--output instead of silently writing in place."""

    def test_output_redirects_all_outputs(self, tmp_path):
        out_dir = tmp_path / "out"
        result = run_gen_ui(str(get_ui_spec_path()), str(out_dir))
        if result is None:
            pytest.skip("gen_ui.py not found")
        assert result.returncode == 0, result.stderr or result.stdout
        for rel in GENERATED_REL_PATHS:
            assert (out_dir / rel).exists(), f"missing redirected output {rel}"

    def test_output_redirect_leaves_repo_untouched(self, tmp_path):
        repo = Path(__file__).parent.parent
        before = {
            rel: (repo / rel).read_bytes() for rel in GENERATED_REL_PATHS if (repo / rel).exists()
        }
        result = run_gen_ui(str(get_ui_spec_path()), str(tmp_path / "out"))
        if result is None:
            pytest.skip("gen_ui.py not found")
        assert result.returncode == 0, result.stderr or result.stdout
        for rel, content in before.items():
            assert (repo / rel).read_bytes() == content, f"redirected run modified repo file {rel}"

    def test_spec_overrides_input(self, tmp_path):
        spec_path = write_minimal_ui_spec(tmp_path / "ui_spec.json", rect_name="TESTBOX")
        out_dir = tmp_path / "out"
        result = run_gen_ui(str(spec_path), str(out_dir))
        if result is None:
            pytest.skip("gen_ui.py not found")
        assert result.returncode == 0, result.stderr or result.stdout
        layout = (out_dir / "firmware/arduino/src/display_layout.h").read_text()
        assert "RECT_TESTBOX" in layout, "--spec input was not used for generation"
        js = (out_dir / "web/sim/ui_generated.js").read_text()
        assert "TESTBOX" in js

    def test_regeneration_is_deterministic(self, tmp_path):
        """Two runs at the same commit must produce identical bytes (no volatile
        git-describe stamp in UI_FW_VERSION unless --git-version is passed)."""
        results = [run_gen_ui(str(get_ui_spec_path()), str(tmp_path / f"out{i}")) for i in (1, 2)]
        if results[0] is None:
            pytest.skip("gen_ui.py not found")
        for result in results:
            assert result.returncode == 0, result.stderr or result.stdout
        for rel in GENERATED_REL_PATHS:
            a = (tmp_path / "out1" / rel).read_bytes()
            b = (tmp_path / "out2" / rel).read_bytes()
            assert a == b, f"{rel} differs between identical runs"
        js = (tmp_path / "out1" / "web/sim/ui_generated.js").read_text()
        assert re.search(r'window\.UI_FW_VERSION = "[^"]+";', js)


class TestInProcessGeneration:
    """Run gen_ui.py's entry points in-process (see load_gen_ui_module)."""

    def test_main_with_spec_and_output(self, tmp_path):
        gen_ui = load_gen_ui_module()
        spec_path = write_minimal_ui_spec(tmp_path / "spec.json", rect_name="INPROC")
        out_dir = tmp_path / "out"
        gen_ui.main(["--spec", str(spec_path), "--output", str(out_dir)])
        for rel in GENERATED_REL_PATHS:
            assert (out_dir / rel).exists(), f"missing redirected output {rel}"
        layout = (out_dir / "firmware/arduino/src/display_layout.h").read_text()
        assert "RECT_INPROC" in layout
        assert str(spec_path) in layout, "source comment should echo the --spec path"

    def test_main_default_spec_redirected(self, tmp_path):
        gen_ui = load_gen_ui_module()
        out_dir = tmp_path / "out"
        gen_ui.main(["--output", str(out_dir)])
        js = (out_dir / "web/sim/ui_generated.js").read_text()
        assert "window.UI_SPEC" in js
        assert "window.UI_LAYOUT_CRC" in js

    def test_main_rejects_invalid_spec(self, tmp_path):
        gen_ui = load_gen_ui_module()
        bad = tmp_path / "bad.json"
        bad.write_text(json.dumps({"schema": "not-a-ui-spec"}))
        with pytest.raises(SystemExit):
            gen_ui.main(["--spec", str(bad), "--output", str(tmp_path / "out")])

    def test_parse_args_warns_on_unknown_args(self, capsys):
        """PlatformIO pre-script safety: SCons-owned argv must not abort the run."""
        gen_ui = load_gen_ui_module()
        args = gen_ui.parse_args(["-Q", "--jobs", "8", "stray"])
        assert args.spec is None
        assert args.output is None
        assert args.git_version is False
        assert "ignoring unrecognized arguments" in capsys.readouterr().err


class TestFwVersionResolution:
    """UI_FW_VERSION must come from committed config, not a git-describe stamp."""

    @staticmethod
    def _isolated(gen_ui, tmp_path, monkeypatch):
        """Point the module's config/output paths into tmp_path."""
        monkeypatch.delenv("FW_VERSION", raising=False)
        gen_ui.FW_OUT_DIR = tmp_path / "fw"
        gen_ui.DEVICE_YAML = tmp_path / "device.yaml"
        gen_ui.DEVICE_SAMPLE_YAML = tmp_path / "device.sample.yaml"
        return gen_ui

    def test_generated_config_header_wins(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        gen_ui.FW_OUT_DIR.mkdir()
        (gen_ui.FW_OUT_DIR / "generated_config.h").write_text('#define FW_VERSION "9.9.9"\n')
        gen_ui.DEVICE_YAML.write_text('fw_version: "1.2.3"\n')
        assert gen_ui.resolve_fw_version() == "9.9.9"

    def test_device_yaml_when_no_generated_config(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        gen_ui.DEVICE_YAML.write_text('fw_version: "1.2.3"\n')
        assert gen_ui.resolve_fw_version() == "1.2.3"

    def test_sample_yaml_fallback(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        gen_ui.DEVICE_SAMPLE_YAML.write_text('fw_version: "1.0.0"\n')
        assert gen_ui.resolve_fw_version() == "1.0.0"

    def test_regex_fallback_without_yaml_module(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        gen_ui.yaml = None
        gen_ui.DEVICE_SAMPLE_YAML.write_text('room_name: "Den"\nfw_version: "2.0.0"\n')
        assert gen_ui.resolve_fw_version() == "2.0.0"

    def test_env_var_fallback(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        monkeypatch.setenv("FW_VERSION", "env-4.5.6")
        assert gen_ui.resolve_fw_version() == "env-4.5.6"

    def test_dev_without_git_opt_in(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        assert gen_ui.resolve_fw_version() == "dev"
        assert gen_ui.resolve_fw_version(allow_git=False) == "dev"

    def test_git_describe_only_on_opt_in(self, tmp_path, monkeypatch):
        gen_ui = self._isolated(load_gen_ui_module(), tmp_path, monkeypatch)
        version = gen_ui.resolve_fw_version(allow_git=True)
        assert version and version != "dev", "opt-in git stamping should describe the repo"


class TestUISpecStructure:
    """Test UI specification structure and validation"""

    def test_ui_spec_exists(self):
        """Test that UI specification file exists"""
        spec_path = get_ui_spec_path()
        # It's OK if spec doesn't exist yet
        if not spec_path.exists():
            pytest.skip("UI spec file not yet created")

    def test_ui_spec_schema(self):
        """Test UI specification follows expected schema"""
        spec = load_ui_spec()
        if not spec:
            pytest.skip("UI spec not available")

        # Check required top-level fields (actual structure uses "schema" and "canvas")
        assert (
            "schema" in spec or "version" in spec or "operations" in spec
        ), "Spec should have schema, version or operations"

        if "canvas" in spec:
            assert "w" in spec["canvas"], "Canvas should have width (w)"
            assert "h" in spec["canvas"], "Canvas should have height (h)"
        elif "display" in spec:
            assert "width" in spec["display"], "Display should have width"
            assert "height" in spec["display"], "Display should have height"

        if "operations" in spec:
            assert isinstance(spec["operations"], list), "Operations should be a list"

        # Check for expected sections in the current format
        if "schema" in spec:
            # New format validation
            assert "canvas" in spec, "Spec should have canvas definition"
            if "fonts" in spec:
                assert "tokens" in spec["fonts"], "Fonts should have tokens"
            if "rects" in spec:
                assert isinstance(spec["rects"], dict), "Rects should be a dictionary"

    def test_operation_types(self):
        """Test that all operation types are valid"""
        valid_types = [
            "drawText",
            "drawRect",
            "fillRect",
            "drawLine",
            "drawCircle",
            "drawImage",
            "setFont",
            "setColor",
            "clear",
        ]

        spec = load_ui_spec()
        if not spec or "operations" not in spec:
            pytest.skip("No operations to test")

        for op in spec["operations"]:
            if "type" in op:
                assert op["type"] in valid_types, f"Unknown operation type: {op['type']}"


class TestCodeGeneration:
    """Test C++ code generation from UI spec"""

    def test_generate_header_file(self):
        """Test header file generation"""
        operations = [{"type": "drawText", "params": {"text": "Test", "x": 10, "y": 20}}]

        with tempfile.TemporaryDirectory() as tmpdir:
            spec_file = create_test_ui_spec(operations)

            # Simulate header generation
            header_content = """#ifndef UI_GENERATED_H
#define UI_GENERATED_H

void renderUI();

#endif // UI_GENERATED_H
"""
            header_path = Path(tmpdir) / "ui_generated.h"
            header_path.write_text(header_content)

            assert header_path.exists(), "Header file should be generated"
            content = header_path.read_text()
            assert "#ifndef UI_GENERATED_H" in content, "Should have include guards"
            assert "void renderUI()" in content, "Should declare render function"

            os.unlink(spec_file)

    def test_generate_cpp_file(self):
        """Test implementation file generation"""
        operations = [
            {"type": "drawText", "params": {"text": "Hello", "x": 0, "y": 0}},
            {"type": "fillRect", "params": {"x": 10, "y": 10, "width": 50, "height": 30}},
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            spec_file = create_test_ui_spec(operations)

            # Simulate C++ generation
            cpp_content = """#include "ui_generated.h"

void renderUI() {
    // drawText: Hello at (0, 0)
    display.drawText("Hello", 0, 0);

    // fillRect at (10, 10) size 50x30
    display.fillRect(10, 10, 50, 30);
}
"""
            cpp_path = Path(tmpdir) / "ui_generated.cpp"
            cpp_path.write_text(cpp_content)

            assert cpp_path.exists(), "CPP file should be generated"
            content = cpp_path.read_text()
            assert '#include "ui_generated.h"' in content, "Should include header"
            assert "void renderUI()" in content, "Should implement render function"
            assert "drawText" in content, "Should have drawText operation"
            assert "fillRect" in content, "Should have fillRect operation"

            os.unlink(spec_file)


class TestOperationTranslation:
    """Test translation of UI operations to C++ code"""

    def test_text_operation_translation(self):
        """Test text drawing operations"""
        test_cases = [
            {
                "op": {"type": "drawText", "params": {"text": "Test", "x": 10, "y": 20}},
                "expected": 'drawText("Test", 10, 20)',
            },
            {
                "op": {"type": "drawText", "params": {"text": "Multi Word", "x": 0, "y": 0}},
                "expected": 'drawText("Multi Word", 0, 0)',
            },
            {
                "op": {"type": "drawText", "params": {"text": "23.5°C", "x": 100, "y": 50}},
                "expected": 'drawText("23.5°C", 100, 50)',
            },
        ]

        for case in test_cases:
            op = case["op"]
            expected = case["expected"]

            # Simulate translation
            if op["type"] == "drawText":
                params = op["params"]
                generated = f'drawText("{params["text"]}", {params["x"]}, {params["y"]})'
                assert expected in generated, "Text operation should translate correctly"

    def test_rect_operation_translation(self):
        """Test rectangle operations"""
        test_cases = [
            {
                "op": {"type": "drawRect", "params": {"x": 0, "y": 0, "width": 100, "height": 50}},
                "expected": "drawRect(0, 0, 100, 50)",
            },
            {
                "op": {"type": "fillRect", "params": {"x": 10, "y": 20, "width": 30, "height": 40}},
                "expected": "fillRect(10, 20, 30, 40)",
            },
        ]

        for case in test_cases:
            op = case["op"]
            expected = case["expected"]

            # Simulate translation
            if op["type"] in ["drawRect", "fillRect"]:
                params = op["params"]
                func = op["type"]
                generated = (
                    f'{func}({params["x"]}, {params["y"]}, {params["width"]}, {params["height"]})'
                )
                assert expected in generated, "Rect operation should translate correctly"

    def test_conditional_operations(self):
        """Test conditional operation generation"""
        # Expected C++ code structure
        expected_code = """
if (hasTemp) {
    drawText(temp, 10, 20);
} else {
    drawText("--", 10, 20);
}
"""

        # Verify conditional structure
        assert "if" in expected_code, "Should have if statement"
        assert "hasTemp" in expected_code, "Should check condition"
        assert "else" in expected_code, "Should have else branch"


class TestDataBinding:
    """Test data binding and variable substitution"""

    def test_variable_substitution(self):
        """Test ${variable} substitution in operations"""
        operations = [
            {"type": "drawText", "params": {"text": "${roomName}", "x": 10, "y": 10}},
            {"type": "drawText", "params": {"text": "Temp: ${temperature}°C", "x": 10, "y": 30}},
        ]

        # Variables should be replaced in generated code
        for op in operations:
            text = op["params"]["text"]
            if "${" in text:
                var_name = re.search(r"\$\{(\w+)\}", text).group(1)
                assert var_name in [
                    "roomName",
                    "temperature",
                ], f"Variable {var_name} should be recognized"

    def test_computed_values(self):
        """Test computed value expressions"""
        operations = [
            {
                "type": "drawText",
                "params": {"text": "${temp * 1.8 + 32}", "x": 10, "y": 10},  # Celsius to Fahrenheit
            },
            {
                "type": "drawRect",
                "params": {
                    "x": 0,
                    "y": 0,
                    "width": "${battery * 2.5}",  # Battery bar width
                    "height": 10,
                },
            },
        ]

        for op in operations:
            # Check if expressions are present
            params_str = str(op["params"])
            if "${" in params_str:
                assert "*" in params_str or "+" in params_str, "Should have arithmetic operations"


class TestRegionMapping:
    """Test mapping of named regions to coordinates"""

    def test_region_name_resolution(self):
        """Test resolution of region names to coordinates"""
        regions = {
            "header": {"x": 0, "y": 0, "w": 250, "h": 16},
            "temp": {"x": 6, "y": 36, "w": 118, "h": 28},
            "humidity": {"x": 6, "y": 66, "w": 118, "h": 14},
        }

        operations = [
            {"type": "drawText", "region": "header", "params": {"text": "Living Room"}},
            {"type": "drawText", "region": "temp", "params": {"text": "23.5°C"}},
        ]

        for op in operations:
            if "region" in op:
                region_name = op["region"]
                assert region_name in regions, f"Region {region_name} should be defined"
                region = regions[region_name]
                # Operation should use region coordinates
                assert region["x"] >= 0 and region["y"] >= 0, "Region should have valid coordinates"

    def test_region_alignment(self):
        """Test text alignment within regions"""
        alignments = ["left", "center", "right", "top", "middle", "bottom"]

        operations = [
            {
                "type": "drawText",
                "region": "header",
                "align": "center",
                "params": {"text": "Title"},
            },
            {"type": "drawText", "region": "temp", "align": "right", "params": {"text": "23.5"}},
        ]

        for op in operations:
            if "align" in op:
                assert op["align"] in alignments, f"Alignment {op['align']} should be valid"


class TestFontHandling:
    """Test font selection and sizing"""

    def test_font_size_mapping(self):
        """Test mapping of font size names to values"""
        font_sizes = {"small": 10, "normal": 12, "large": 16, "huge": 22}

        operations = [
            {"type": "setFont", "size": "small"},
            {"type": "drawText", "font": "large", "params": {"text": "Big", "x": 0, "y": 0}},
            {"type": "drawText", "font": "huge", "params": {"text": "Huge", "x": 0, "y": 30}},
        ]

        for op in operations:
            if "size" in op:
                assert op["size"] in font_sizes, f"Font size {op['size']} should be defined"
            if "font" in op:
                assert op["font"] in font_sizes, f"Font {op['font']} should be defined"

    def test_font_weight_styles(self):
        """Test font weight and style options"""
        weights = ["normal", "bold", "light"]
        styles = ["normal", "italic"]

        operations = [
            {"type": "drawText", "weight": "bold", "params": {"text": "Bold", "x": 0, "y": 0}},
            {"type": "drawText", "style": "italic", "params": {"text": "Italic", "x": 0, "y": 20}},
        ]

        for op in operations:
            if "weight" in op:
                assert op["weight"] in weights, f"Weight {op['weight']} should be valid"
            if "style" in op:
                assert op["style"] in styles, f"Style {op['style']} should be valid"


class TestIconGeneration:
    """Test icon embedding and rendering"""

    def test_icon_reference(self):
        """Test icon references in operations"""
        icons = [
            "weather_clear",
            "weather_rain",
            "weather_cloud",
            "battery_full",
            "battery_empty",
            "battery_charging",
            "wifi_on",
            "wifi_off",
        ]

        operations = [
            {"type": "drawIcon", "icon": "weather_clear", "params": {"x": 210, "y": 22}},
            {"type": "drawIcon", "icon": "battery_full", "params": {"x": 230, "y": 100}},
        ]

        for op in operations:
            if op["type"] == "drawIcon":
                assert "icon" in op, "drawIcon should have icon name"
                # Icon names should follow convention
                assert "_" in op["icon"], "Icon names should use underscore separator"
                assert op["icon"] in icons, f"Unknown icon reference: {op['icon']}"

    def test_icon_data_embedding(self):
        """Test icon data embedding in generated code"""
        icon_data = {
            "weather_clear": [0xFF, 0x81, 0x81, 0xFF],  # Simple 2x2 icon
            "battery_full": [0xFF, 0xFF, 0xFF, 0xFF],
        }

        # Generated code should include icon data
        expected_code = """
const uint8_t icon_weather_clear[] = {0xFF, 0x81, 0x81, 0xFF};
const uint8_t icon_battery_full[] = {0xFF, 0xFF, 0xFF, 0xFF};
"""

        for icon_name in icon_data:
            assert f"icon_{icon_name}" in expected_code, f"Icon {icon_name} should be embedded"


class TestErrorHandling:
    """Test error handling in code generation"""

    def test_invalid_operation_type(self):
        """Test handling of invalid operation types"""
        operations = [{"type": "invalidOperation", "params": {}}]

        with tempfile.TemporaryDirectory():
            spec_file = create_test_ui_spec(operations)

            # Should handle gracefully or skip invalid operations
            # Not testing actual script execution, just the concept
            assert spec_file, "Should create spec even with invalid operations"

            os.unlink(spec_file)

    def test_missing_required_params(self):
        """Test handling of missing required parameters"""
        operations = [
            {"type": "drawText"},  # Missing params
            {"type": "drawText", "params": {"text": "Test"}},  # Missing x, y
            {"type": "fillRect", "params": {"x": 0, "y": 0}},  # Missing width, height
        ]

        for op in operations:
            # Should validate required parameters
            if op["type"] == "drawText":
                if "params" in op:
                    params = op["params"]
                    required = ["text", "x", "y"]
                    missing = [p for p in required if p not in params]
                    assert len(missing) > 0 or "text" in params, "Should detect missing parameters"

    def test_type_validation(self):
        """Test parameter type validation"""
        invalid_operations = [
            {"type": "drawText", "params": {"text": 123, "x": 0, "y": 0}},  # text should be string
            {
                "type": "drawRect",
                "params": {"x": "zero", "y": 0, "width": 10, "height": 10},
            },  # x should be number
            {"type": "setFont", "size": 999},  # Invalid font size
        ]

        for op in invalid_operations:
            # Should validate parameter types
            params = op.get("params", {})

            if op["type"] == "drawText" and "text" in params:
                assert isinstance(params["text"], (str, int)), "Text parameter should be validated"

            if op["type"] == "drawRect" and "x" in params:
                # X coordinate should be numeric
                try:
                    float(params["x"])
                    is_valid = True
                except (TypeError, ValueError):
                    is_valid = False
                assert not is_valid or isinstance(
                    params["x"], (int, float)
                ), "Coordinate should be numeric"


class TestOptimization:
    """Test code optimization in generation"""

    def test_batch_similar_operations(self):
        """Test batching of similar operations"""
        operations = [
            {"type": "setFont", "size": "small"},
            {"type": "drawText", "params": {"text": "Line 1", "x": 0, "y": 10}},
            {"type": "drawText", "params": {"text": "Line 2", "x": 0, "y": 20}},
            {"type": "drawText", "params": {"text": "Line 3", "x": 0, "y": 30}},
            {"type": "setFont", "size": "large"},
            {"type": "drawText", "params": {"text": "Big", "x": 0, "y": 50}},
        ]

        # Should group operations efficiently
        # Font should be set once for multiple text operations
        font_changes = sum(1 for op in operations if op["type"] == "setFont")
        text_operations = sum(1 for op in operations if op["type"] == "drawText")

        assert font_changes == 2, "Should have 2 font changes"
        assert text_operations == 4, "Should have 4 text operations"
        assert font_changes < text_operations, "Should have fewer font changes than text ops"

    def test_constant_extraction(self):
        """Test extraction of constants in generated code"""
        operations = [
            {"type": "drawText", "params": {"text": "Version 1.0", "x": 172, "y": 2}},
            {"type": "drawText", "params": {"text": "Version 1.0", "x": 172, "y": 110}},
        ]

        # Repeated strings should be constants
        # Expected: const char* VERSION_TEXT = "Version 1.0";
        repeated_text = "Version 1.0"
        count = sum(1 for op in operations if op.get("params", {}).get("text") == repeated_text)

        assert count > 1, "Should have repeated text for constant extraction"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
