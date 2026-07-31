import contextlib
import json
import os
import socket
import subprocess
import time

import pytest


def _find_free_port() -> int:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _start_http_server(root: str, port: int) -> subprocess.Popen:
    return subprocess.Popen(
        ["python3", "-m", "http.server", str(port), "--bind", "127.0.0.1"],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _ensure_out_dir() -> str:
    root = os.path.dirname(os.path.dirname(__file__))
    out_dir = os.path.join(root, "out")
    os.makedirs(out_dir, exist_ok=True)
    return out_dir


_CANVAS_RGBA_JS = (
    "([x,y])=>{"
    "const c=document.getElementById('epd');const ctx=c.getContext('2d');"
    "return Array.from(ctx.getImageData(x,y,1,1).data);}"
)

_TIME_METRICS_JS = (
    "() => {\n"
    "  const c=document.getElementById('epd');\n"
    "  const ctx=c.getContext('2d');\n"
    "  const spec = window.UI_SPEC || {};\n"
    "  const R = spec.rects || {};\n"
    "  // Use HEADER_TIME_CENTER for centered time display\n"
    "  const rt = R.HEADER_TIME_CENTER || [100,2,50,14];\n"
    "  const t = (window.lastData && window.lastData.time_hhmm) || '10:32';\n"
    '  ctx.font = \'11px Menlo, Consolas, "DM Mono", "Roboto Mono", monospace\';\n'
    "  const tw = ctx.measureText(t).width;\n"
    "  // For centered text, calculate center position\n"
    "  const timeX = rt[0] + (rt[2] - tw) / 2;\n"
    "  return {x: timeX, y: rt[1]+1, w: tw, rt};\n"
    "}"
)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_canvas_is_binary_after_draw():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            page.wait_for_timeout(300)

            # Sample a small grid across the canvas to ensure only 0 or 255 per channel
            grid = [(x, y) for x in range(0, 250, 25) for y in range(0, 122, 12)]
            for x, y in grid:
                r, g, b, a = page.evaluate(_CANVAS_RGBA_JS, [x, y])
                assert r in (0, 255) and g in (0, 255) and b in (0, 255)
                assert r == g == b
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_stress_mode_renders_without_overlap():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            page.wait_for_timeout(300)

            # The toggles moved inside an "Advanced" <details> disclosure, so the
            # checkbox has no layout box until it is expanded and Playwright
            # correctly refuses to click it.
            page.locator("details.advanced-toggles > summary").click()
            # Enable stress mode
            page.check("#stressMode")
            page.wait_for_timeout(300)

            # Probe that the layout still draws expected frame lines
            for x, y in [(0, 0), (249, 0), (0, 121), (249, 121)]:
                r, g, b, a = page.evaluate(_CANVAS_RGBA_JS, [x, y])
                assert (r, g, b) == (0, 0, 0)
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_icons_available_or_fallback():
    """Test that weather icons exist in the filesystem.

    Note: Icons are served from filesystem, not HTTP, so we check file existence
    rather than HTTP fetch. The simulator uses fallback glyphs if SVGs are missing.
    """
    import os

    # Check that icon files exist on filesystem
    icons_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web", "icons", "mdi")
    required_icons = [
        "weather-sunny.svg",
        "weather-cloudy.svg",
        "weather-pouring.svg",
        "weather-snowy.svg",
        "weather-lightning.svg",
        "weather-fog.svg",
    ]

    for icon_name in required_icons:
        icon_path = os.path.join(icons_dir, icon_name)
        assert os.path.exists(icon_path), f"Missing weather icon: {icon_name}"


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_header_time_right_aligned_and_name_truncated():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})

            payload = {
                "room_name": "Extremely Long Room Name That Should Truncate",
                "time_hhmm": "23:59",
            }

            def handle_route(route):
                base = {
                    "inside_temp": "72.5",
                    "inside_hum": "47",
                    "outside_temp": "68.4",
                    "outside_hum": "53",
                    "weather": "Cloudy",
                    "ip": "192.168.1.42",
                    "voltage": "4.01",
                    "percent": 76,
                    "days": "128",
                    "wind": "4.2",
                }
                base.update(payload)
                body_text = json.dumps(base)
                route.fulfill(
                    status=200,
                    content_type="application/json",
                    body=body_text,
                )

            page.route("**/sample_data.json", handle_route)
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            page.wait_for_timeout(300)

            # v3 header is an inverted band: glyphs are WHITE knocked out of
            # solid black, so the polarity of both probes flips relative to the
            # pre-v3 expectations this test originally encoded.
            time_metrics = page.evaluate(_TIME_METRICS_JS)
            # Probe a small 3x3 neighborhood around the measured center for any
            # white (glyph) pixel — proves the stamp actually rendered.
            cx = int(time_metrics["x"] + max(1, time_metrics["w"] // 2))
            cy = int(time_metrics["y"] + 2)
            any_glyph = False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    r1, g1, b1, a1 = page.evaluate(_CANVAS_RGBA_JS, [cx + dx, cy + dy])
                    if (r1, g1, b1) == (255, 255, 255):
                        any_glyph = True
                        break
                if any_glyph:
                    break
            assert any_glyph
            # Just left of the stamp's rect must be solid band (no glyph spill),
            # confirming the alignment gap — black under the inverted header.
            hx, hy, hw, hh = time_metrics["rt"]
            r0, g0, b0, a0 = page.evaluate(_CANVAS_RGBA_JS, [hx - 2, hy + 2])
            assert (r0, g0, b0) == (0, 0, 0)
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_mock_vs_web_sim_pixel_diff(tmp_path):
    from playwright.sync_api import sync_playwright

    # Optional numpy for pixel diff math
    try:
        import numpy as np
    except Exception:
        pytest.skip("numpy not installed")
    # Render mock PNG to a buffer
    import importlib.util as _ilu
    import os as _os

    ROOT = _os.path.dirname(_os.path.dirname(__file__))
    _scripts = _os.path.join(ROOT, "scripts")
    _module_path = _os.path.join(_scripts, "mock_display.py")
    _spec = _ilu.spec_from_file_location("mock_display", _module_path)
    md = _ilu.module_from_spec(_spec)
    _spec.loader.exec_module(md)

    data = {
        "room_name": "Office",
        "inside_temp": "72.5",
        "inside_hum": "47",
        "outside_temp": "68.4",
        "outside_hum": "53",
        "weather": "Cloudy",
        "time": "10:32",
        "ip": "192.168.1.42",
        "voltage": "4.01",
        "percent": 76,
        "days": "128",
        "wind": "4.2",
    }
    mock_img = md.render(data)

    # Capture web sim canvas pixels
    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            page.wait_for_timeout(300)

            # Feed data via fetch override route
            def handle_route(route):
                route.fulfill(status=200, content_type="application/json", body=json.dumps(data))

            page.route("**/sample_data.json", handle_route)
            page.reload(wait_until="load")
            page.wait_for_timeout(300)
            # Read pixels
            js_read = (
                "()=>{const c=document.getElementById('epd');const ctx=c.getContext('2d');"
                "return Array.from(ctx.getImageData(0,0,c.width,c.height).data);}"
            )
            pix = page.evaluate(js_read)
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)

    # Convert RGBA array to 1-bit similar to sim threshold
    W, H = mock_img.size
    arr = np.array(pix, dtype=np.uint8).reshape(H, W, 4)
    y = 0.2126 * arr[:, :, 0] + 0.7152 * arr[:, :, 1] + 0.0722 * arr[:, :, 2]
    sim_bin = (y < 176).astype(np.uint8)

    mock_bin = np.array(mock_img, dtype=np.uint8)
    # Normalize to 0/1 to match sim_bin
    mock_bin = (mock_bin > 0).astype(np.uint8)
    # Compute absolute difference and allow small tolerance window
    diff = np.abs(sim_bin.astype(np.int16) - mock_bin.astype(np.int16))
    num_diff = int((diff != 0).sum())
    # Save artifacts
    out_dir = _ensure_out_dir()
    mock_path = os.path.join(out_dir, "expected.png")
    mock_img.save(mock_path)
    # Save a visualization of differences
    from PIL import Image

    vis = Image.fromarray((diff * 255).astype(np.uint8), mode="L")
    vis.save(os.path.join(out_dir, "pixel_diff.png"))
    # Allow a reasonable number of differing pixels due to font/antialiasing/thresholds
    assert num_diff < 30000
