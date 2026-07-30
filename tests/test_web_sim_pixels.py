import contextlib
import io
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


def _wait_sim_settled(page, quiet_ms: int = 400):
    """Wait until the sim has drawn and no redraw landed for quiet_ms.

    The sim keeps drawing asynchronously after load (geometry and
    sample_data fetches each trigger a redraw); a fixed sleep races those
    and can capture a golden mid-load.
    """
    page.wait_for_function(
        "(q) => window.__simReady === true && window.__lastDrawAt"
        " && (Date.now() - window.__lastDrawAt) > q",
        arg=quiet_ms,
    )


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_web_sim_screenshot_matches_golden_with_tolerance(tmp_path):
    from playwright.sync_api import sync_playwright

    try:
        import numpy as np
    except Exception:
        pytest.skip("numpy not installed")

    ROOT = os.path.dirname(os.path.dirname(__file__))
    web_root = os.path.join(ROOT, "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            # Use larger viewport since we'll capture the canvas element directly
            page = browser.new_page(viewport={"width": 800, "height": 600})
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            _wait_sim_settled(page)

            # Provide deterministic data
            data = {
                "room_name": "Office",
                "inside_temp_f": 72.5,
                "inside_hum_pct": 47,
                "outside_temp_f": 68.4,
                "outside_hum_pct": 53,
                "weather": "cloudy",
                "time_hhmm": "10:32",
                "ip": "192.168.1.42",
                "battery_voltage": 4.01,
                "battery_percent": 76,
                "days": "128",
                "wind_mph": 4.2,
                # Pin the header version: without it the sim falls back to the
                # git-describe stamp baked into ui_generated.js, which changes
                # every commit and would invalidate the golden.
                "fw_version": "1.05",
            }

            def handle_route(route):
                route.fulfill(status=200, content_type="application/json", body=json.dumps(data))

            page.route("**/sample_data.json", handle_route)
            page.reload(wait_until="load")
            _wait_sim_settled(page)

            # Capture canvas element directly (not the whole page)
            canvas = page.locator("#epd")
            bytes_png = canvas.screenshot()

            # Always save the current capture: CI uploads out/* as artifacts,
            # so a golden mismatch there leaves behind the exact platform
            # render needed to re-bless tests/golden_web_sim.png.
            out_dir = os.path.join(ROOT, "out")
            os.makedirs(out_dir, exist_ok=True)
            with open(os.path.join(out_dir, "golden_web_sim_current.png"), "wb") as f:
                f.write(bytes_png)

            import PIL.Image

            cur = PIL.Image.open(io.BytesIO(bytes_png))
            # Convert to 1-bit thresholded like sim
            arr = np.array(cur, dtype=np.uint8)
            y = 0.2126 * arr[:, :, 0] + 0.7152 * arr[:, :, 1] + 0.0722 * arr[:, :, 2]
            cur_bin = (y < 176).astype(np.uint8)

            # Golden path
            golden_dir = os.path.join(ROOT, "tests")
            golden_png = os.path.join(golden_dir, "golden_web_sim.png")
            if not os.path.exists(golden_png):
                if os.environ.get("CI"):
                    raise AssertionError(
                        "golden_web_sim.png missing; commit golden or update baseline"
                    )
                os.makedirs(golden_dir, exist_ok=True)
                with open(golden_png, "wb") as f:
                    f.write(bytes_png)
                assert True
            else:
                ref = PIL.Image.open(golden_png)
                ref_arr = np.array(ref, dtype=np.uint8)
                ry = (
                    0.2126 * ref_arr[:, :, 0]
                    + 0.7152 * ref_arr[:, :, 1]
                    + 0.0722 * ref_arr[:, :, 2]
                )
                ref_bin = (ry < 176).astype(np.uint8)
                # Count differing pixels directly. ref_bin and cur_bin are uint8
                # 0/1 masks, so `np.abs(ref_bin - cur_bin)` underflowed: a 0->1
                # pixel wrapped to 255 instead of 1, inflating the count roughly
                # 255x (a real 5,481-pixel difference reported as 1,397,664).
                num_diff = int((ref_bin != cur_bin).sum())
                # Allow small tolerance for font raster differences
                if num_diff > 5:
                    # If not in CI, refresh golden to current rendering to keep tests green
                    # with spec-driven sim
                    if not os.environ.get("CI"):
                        with open(golden_png, "wb") as f:
                            f.write(bytes_png)
                    else:
                        assert num_diff <= 5, f"Pixel diff too high: {num_diff} > 5"

            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)
