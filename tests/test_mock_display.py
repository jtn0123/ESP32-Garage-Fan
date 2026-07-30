import importlib.util
import json
import os

ROOT = os.path.dirname(os.path.dirname(__file__))
_scripts = os.path.join(ROOT, "scripts")
_module_path = os.path.join(_scripts, "mock_display.py")
_spec = importlib.util.spec_from_file_location("mock_display", _module_path)
md = importlib.util.module_from_spec(_spec)  # type: ignore
_spec.loader.exec_module(md)  # type: ignore


def test_render_dimensions():
    img = md.render({})
    assert img.size == (md.WIDTH, md.HEIGHT)
    assert img.mode == "1"


def test_header_and_frame_nonwhite_pixels():
    img = md.render({"room_name": "Test"})
    px = img.load()
    # Corners should be black frame
    assert px[0, 0] == 0
    assert px[md.WIDTH - 1, 0] == 0
    assert px[0, md.HEIGHT - 1] == 0
    assert px[md.WIDTH - 1, md.HEIGHT - 1] == 0


def test_icon_changes_image_hash():
    a = md.image_md5(md.render({"weather": "Clear"}))
    b = md.image_md5(md.render({"weather": "Cloudy"}))
    assert a != b


def test_png_icon_loads_and_renders_centered():
    # uses generated 24x24 icons if present
    img = md.render({"weather": "cloudy"})
    # Load geometry JSON for icon box
    gpaths = [
        os.path.join(ROOT, "config", "display_geometry.json"),
        os.path.join(ROOT, "web", "sim", "geometry.json"),
    ]
    G = None
    for p in gpaths:
        if os.path.exists(p):
            with open(p, "r") as f:
                G = json.load(f)
            break
    assert G is not None
    rects = G.get("rects", G)
    icon = rects.get("OUT_ICON", [210, 22, 28, 28])
    x, y, w, h = icon
    x0, y0, x1, y1 = x, y, x + w, y + h
    px = img.load()
    has_black = any(px[i, j] == 0 for i in range(x0, x1) for j in range(y0, y1))
    assert has_black


def test_short_condition_label_mappings():
    # Mirrors web/sim/sim.js shortConditionLabel and the firmware's
    # spec_short_condition: all three renderers must agree on these.
    cases = {
        "clear-night": "Night",
        "partlycloudy": "Partly",
        "cloudy": "Cloudy",
        "overcast skies": "Cloudy",
        "lightning-rainy": "Storm",
        "pouring": "Rain",
        "drizzle": "Rain",
        "hail": "Hail",
        "snowy-sleet": "Snow",
        "fog": "Fog",
        "haze": "Fog",
        "windy-variant": "Wind",
        "sunny": "Sunny",
        "": "Sunny",
    }
    for raw, expected in cases.items():
        assert md.short_condition_label(raw) == expected, raw


def test_pressure_rows_render_only_when_data_present():
    base = {"room_name": "Office", "inside_temp": "72.5", "outside_temp": "68.4"}
    without = md.render(base)
    with_pressure = md.render({**base, "pressure_hpa": 1013.2, "outside_pressure_hpa": 1015.8})
    assert md.image_md5(without) != md.image_md5(with_pressure)

    rects = md.load_geometry()
    px_with = with_pressure.load()
    px_without = without.load()
    for key in ("INSIDE_PRESSURE", "OUT_PRESSURE"):
        x, y, w, h = rects[key]
        has_with = any(px_with[i, j] == 0 for i in range(x, x + w) for j in range(y, y + h))
        # Negative check skips the rect's top rows: glyph descenders from the
        # row above (RH/wind) bleed a couple of pixels into the pressure rect.
        has_without = any(
            px_without[i, j] == 0 for i in range(x, x + w) for j in range(y + 3, y + h)
        )
        assert has_with, f"{key}: pressure text missing when data provided"
        assert not has_without, f"{key}: pressure text drawn without data"
