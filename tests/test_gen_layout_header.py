import importlib.util
import json
import os

ROOT = os.path.dirname(os.path.dirname(__file__))
_scripts = os.path.join(ROOT, "scripts")
_module_path = os.path.join(_scripts, "gen_layout_header.py")
_spec = importlib.util.spec_from_file_location("gen_layout_header", _module_path)
glh = importlib.util.module_from_spec(_spec)  # type: ignore
_spec.loader.exec_module(glh)  # type: ignore


def test_generate_header_basic_contents():
    data = {
        "canvas": {"w": 250, "h": 122},
        "rects": {
            "HEADER_NAME": [6, 2, 160, 14],
            "HEADER_TIME": [172, 2, 72, 14],
            "STATUS": [6, 112, 238, 10],
        },
    }
    out = glh.generate_header(data)
    assert "#define EINK_WIDTH 250" in out
    assert "#define EINK_HEIGHT 122" in out
    # STATUS is renamed to STATUS_
    assert "static constexpr int STATUS_[4]" in out
    # md5 line present
    assert "// md5:" in out
    # sanity checks emitted for windows
    assert "static_assert(" in out


def test_main_honors_geometry_and_out(tmp_path):
    geometry = {
        "canvas": {"w": 250, "h": 122},
        "rects": {"HEADER_NAME": [6, 2, 160, 14]},
    }
    geometry_path = tmp_path / "geometry.json"
    geometry_path.write_text(json.dumps(geometry))
    out_path = tmp_path / "display_layout.h"
    glh.main(["--geometry", str(geometry_path), "--out", str(out_path)])
    text = out_path.read_text()
    assert "#define DISPLAY_WIDTH 250" in text
    assert "HEADER_NAME" in text


def test_main_missing_geometry_reports_error(tmp_path, capsys):
    out_path = tmp_path / "display_layout.h"
    glh.main(["--geometry", str(tmp_path / "missing.json"), "--out", str(out_path)])
    assert "ERR: geometry json not found" in capsys.readouterr().out
    assert not out_path.exists()


def test_parse_args_warns_on_unknown_args(capsys):
    """PlatformIO pre-script safety: SCons-owned argv must not abort the run."""
    args = glh.parse_args(["-Q", "stray"])
    assert args.geometry is None
    assert args.out is None
    assert "ignoring unrecognized arguments" in capsys.readouterr().err
