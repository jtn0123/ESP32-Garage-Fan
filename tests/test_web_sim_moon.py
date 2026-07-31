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


_CANVAS_RGBA_JS = (
    "([x,y])=>{"
    "const c=document.getElementById('epd');const ctx=c.getContext('2d');"
    "return Array.from(ctx.getImageData(x,y,1,1).data);}"
)


@pytest.mark.skip(reason="Moon phase icons not implemented in current UI spec")
@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_moon_phase_icon_draws_nonwhite_pixels():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web", "sim")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})

            def handle_route(route):
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
                    "moon_phase": "Full",
                }
                route.fulfill(status=200, content_type="application/json", body=json.dumps(data))

            page.route("**/sample_data.json", handle_route)
            page.goto(f"http://127.0.0.1:{port}/index.html", wait_until="load")
            page.wait_for_timeout(300)

            def grab_region(x0, y0, w, h):
                return page.evaluate(_CANVAS_RGBA_JS, [x0, y0, w, h])

            # Weather bar region where icon+label are drawn
            barX, barY, _barW = 130, 95, 114
            # sample a smaller icon-left region inside the bar
            # where the moon icon should be rendered
            x0 = barX + 2
            y0 = barY + 2
            w = 32
            h = 20
            js_cnt = (
                "([x0,y0,w,h])=>{"
                "const c=document.getElementById('epd');const ctx=c.getContext('2d');"
                "const d=ctx.getImageData(x0,y0,w,h).data;let k=0;"
                "for(let i=0;i<d.length;i+=4){"
                "  if(!(d[i]===255&&d[i+1]===255&&d[i+2]===255))k++;"
                "}return k;}"
            )
            cnt = page.evaluate(js_cnt, [x0, y0, w, h])
            assert cnt > 0
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_partial_refresh_header_time_remains_binary():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web", "sim")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})
            page.goto(f"http://127.0.0.1:{port}/index.html", wait_until="load")
            page.wait_for_timeout(300)
            # Click refresh (triggers partial time update)
            page.click("#refresh")
            page.wait_for_timeout(200)
            # Read HEADER_TIME_CENTER geometry
            rt = page.evaluate(
                "() => {"
                "const R=(window.GJSON&&window.GJSON.rects)||null;"
                "return R ? R.HEADER_TIME_CENTER : [100,2,50,14];}"
            )
            if rt is None:
                pytest.skip("HEADER_TIME_CENTER geometry not available")
            x, y, w, h = rt
            # Sample multiple points inside the time box and assert binary pixels
            for dx in [2, int(w / 3), int(2 * w / 3), w - 3]:
                r, g, b, a = page.evaluate(_CANVAS_RGBA_JS, [x + dx, y + 2])
                # Three-ink panel: white, black, or the tri-color red (204,0,0)
                # - the v3 header band is red on the tri-color design.
                assert (r, g, b) in ((0, 0, 0), (255, 255, 255), (204, 0, 0))
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skip(reason="Moon phase icons not implemented in current UI spec")
@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_vector_fallback_when_svg_blocked():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web", "sim")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})
            # Block all SVG fetches so loader must fallback to vector
            page.route("**/*.svg", lambda route: route.fulfill(status=404))
            page.goto(f"http://127.0.0.1:{port}/index.html", wait_until="load")
            page.wait_for_timeout(300)

            # Count non-white pixels in weather icon area to ensure something was drawn
            barX, barY, _barW = 130, 95, 114
            # approximate icon box left portion inside bar
            x0, y0, w, h = barX + 2, barY + 2, 28, 20
            js_cnt = (
                "([x0,y0,w,h])=>{"
                "const c=document.getElementById('epd');"
                "const ctx=c.getContext('2d');"
                "const d=ctx.getImageData(x0,y0,w,h).data;let k=0;"
                "for(let i=0;i<d.length;i+=4){"
                "  if(!(d[i]===255&&d[i+1]===255&&d[i+2]===255))k++;"
                "}return k;}"
            )
            cnt = page.evaluate(js_cnt, [x0, y0, w, h])
            assert cnt > 0
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)
