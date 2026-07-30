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


def _wait_sim_settled(page, quiet_ms: int = 400):
    """Wait until the sim has drawn and no redraw landed for quiet_ms.

    The sim keeps drawing asynchronously after load (geometry and
    sample_data fetches each trigger a redraw); a fixed sleep races those.
    """
    page.wait_for_function(
        "(q) => window.__simReady === true && window.__lastDrawAt"
        " && (Date.now() - window.__lastDrawAt) > q",
        arg=quiet_ms,
    )


def _canvas_rgba(page, x: int, y: int):
    js = (
        "([x,y])=>{"
        "const c=document.getElementById('epd');"
        "const ctx=c.getContext('2d');"
        "return Array.from(ctx.getImageData(x,y,1,1).data);}"
    )
    return page.evaluate(js, [x, y])


def _ensure_out_dir() -> str:
    root = os.path.dirname(os.path.dirname(__file__))
    out_dir = os.path.join(root, "out")
    os.makedirs(out_dir, exist_ok=True)
    return out_dir


def _save_artifacts(page, name: str = "sim") -> None:
    out_dir = _ensure_out_dir()
    # Save full page and canvas-only screenshots plus layout metrics JSON
    page.screenshot(path=os.path.join(out_dir, f"{name}_page.png"))
    canvas = page.query_selector("#epd")
    if canvas:
        canvas.screenshot(path=os.path.join(out_dir, f"{name}_canvas.png"))
    metrics = page.evaluate(
        "() => ({ L: window.__layoutMetrics || null, T: window.__tempMetrics || null })"
    )
    with open(os.path.join(out_dir, f"{name}_metrics.json"), "w") as f:
        json.dump(metrics, f, indent=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_layout_centering_and_clipping():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 700, "height": 400})
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            _wait_sim_settled(page)

            # Ensure canvas logs exist
            assert page.evaluate("() => !!document.getElementById('epd')")

            # Optional always-save screenshots for visual review
            if os.getenv("SIM_SAVE_SHOT") or os.getenv("SAVE_SHOT"):
                _save_artifacts(page, name="boot")

            # Query computed geometry and derived centers from the page context
            metrics = page.evaluate(
                "() => {\n"
                "  const ctx = document.getElementById('epd').getContext('2d');\n"
                "  const M = {};\n"
                "  M.sampleData = window.lastData || null;\n"
                "  M.canvas = {w: ctx.canvas.width, h: ctx.canvas.height};\n"
                "  return M;\n"
                "}"
            )
            assert metrics["canvas"]["w"] == 250 and metrics["canvas"]["h"] == 122

            # Simple smoke: corners must be black (frame)
            for x, y in [(0, 0), (249, 0), (0, 121), (249, 121)]:
                r, g, b, a = _canvas_rgba(page, x, y)
                assert (r, g, b) == (0, 0, 0)

            # Pull exported layout metrics to validate guidelines
            M = page.evaluate("() => window.__layoutMetrics || null")
            assert M is not None

            # Footer rects from the active spec (single source of truth)
            rects = page.evaluate("() => (window.UI_SPEC && window.UI_SPEC.rects) || null")
            assert rects is not None
            bat_rect = rects["FOOTER_BATTERY"]  # [x, y, w, h]
            ip_rect = rects["FOOTER_IP"]

            try:
                # 1) Battery icon vertically centered within FOOTER_BATTERY
                icon = M["statusLeft"]["batteryIcon"]
                icon_cy = icon["y"] + icon["h"] / 2
                rect_cy = bat_rect[1] + bat_rect[3] / 2
                assert abs(icon_cy - rect_cy) <= 2

                # 2) Battery text right-aligned in FOOTER_BATTERY, on the same row
                #    as the icon, without overlapping it
                bt = M["statusLeft"]["batteryText"]
                text_right = bt["x"] + bt["w"]
                rect_right = bat_rect[0] + bat_rect[2]
                assert abs(text_right - (rect_right - 2)) <= 2
                assert icon["x"] + icon["w"] + 2 <= bt["x"]
                row1y = M["statusLeft"]["line1Y"]
                assert bat_rect[1] <= row1y <= bat_rect[1] + bat_rect[3]

                # 3) IP row centered within FOOTER_IP
                ipx = M["statusLeft"]["ip"]["x"]
                ipw = M["statusLeft"]["ip"]["w"]
                ip_mid = ipx + ipw / 2
                col_mid = ip_rect[0] + ip_rect[2] / 2
                assert abs(ip_mid - col_mid) <= 1.5

                # 4) Weather block (icon + label) is horizontally centered (legacy layout only)
                if "bar" in M.get("weather", {}):
                    wx = M["weather"]["bar"]["x"]
                    ww = M["weather"]["bar"]["w"]
                    totalW = M["weather"]["totalW"]
                    block_left = M["weather"]["iconBox"]["x"]
                    block_mid = block_left + totalW / 2
                    bar_mid = wx + ww / 2
                    assert abs(block_mid - bar_mid) <= 1.5

                # 5) Temperature groups (inside/outside) roughly centered
                T = page.evaluate("() => window.__tempMetrics || null")
                assert T is not None
                for key in ["inside", "outside"]:
                    r = T[key]["rect"]
                    contentLeft = T[key]["contentLeft"]
                    totalW = T[key]["totalW"]
                    mid = r["x"] + r["w"] / 2
                    groupMid = contentLeft + totalW / 2
                    assert abs(mid - groupMid) <= 2

                # 6) Section labels centered above their temp blocks (if using labeled layout)
                if "inside" in M.get("labels", {}) and "outside" in M.get("labels", {}):

                    def _centered_over(rect, label_x, tol=2):
                        mid = rect["x"] + rect["w"] / 2
                        assert abs(label_x - mid) <= tol

                    _centered_over(T["inside"]["rect"], M["labels"]["inside"]["x"])
                    _centered_over(T["outside"]["rect"], M["labels"]["outside"]["x"])
            except AssertionError:
                _save_artifacts(page, name="failure")
                raise

            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_web_sim_backend_integration_full_reload():
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})

            # Serve two different backend payloads on successive requests
            payloads = [
                {
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
                },
                {
                    "room_name": "Office",
                    "inside_temp": "72.5",
                    "inside_hum": "47",
                    "outside_temp": "68.4",
                    "outside_hum": "53",
                    "weather": "Rain",
                    "time": "10:32",
                    "ip": "192.168.1.42",
                    "voltage": "4.01",
                    "percent": 76,
                    "days": "128",
                    "wind": "4.2",
                },
            ]
            idx = {"i": 0}

            def handle_route(route):
                if idx["i"] < len(payloads):
                    data = payloads[idx["i"]]
                    idx["i"] += 1
                else:
                    data = payloads[-1]
                route.fulfill(status=200, content_type="application/json", body=json.dumps(data))

            page.route("**/sample_data.json", handle_route)

            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            _wait_sim_settled(page)

            # Count non-white pixels in the bottom-right weather bar area
            def count_nonwhite(x0, y0, w, h):
                js_count = (
                    "([x0,y0,w,h])=>{"
                    "const c=document.getElementById('epd');const ctx=c.getContext('2d');"
                    "const d=ctx.getImageData(x0,y0,w,h).data;let cnt=0;"
                    "for(let i=0;i<d.length;i+=4){"
                    "if(!(d[i]===255&&d[i+1]===255&&d[i+2]===255))cnt++;}return cnt;}"
                )
                return page.evaluate(js_count, [x0, y0, w, h])

            area = (130, 95, 114, 24)
            cnt_cloudy = count_nonwhite(*area)

            # Full reload → second payload (Rain)
            page.reload(wait_until="load")
            _wait_sim_settled(page)
            cnt_rain = count_nonwhite(*area)

            # Expect a visual change in the weather area between Cloudy and Rain
            assert cnt_cloudy != cnt_rain
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_web_sim_partial_refresh_only_updates_header_time():
    """Test that refresh button redraws the display with new data.

    Note: Current simulator does full redraw on refresh, not partial refresh.
    This test validates that the refresh mechanism works and updates the display.
    """
    from playwright.sync_api import sync_playwright

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})

            # First payload for initial load; second payload changes outside_temp
            payloads = [
                {"outside_temp_f": 68.4},
                {"outside_temp_f": 100.0},
            ]
            idx = {"i": 0}

            def handle_route(route):
                if idx["i"] < len(payloads):
                    base = {
                        "room_name": "Office",
                        "inside_temp_f": 72.5,
                        "inside_hum_pct": 47,
                        "outside_hum_pct": 53,
                        "weather": "cloudy",
                        "time_hhmm": "10:32",
                        "ip": "192.168.1.42",
                        "battery_voltage": 4.01,
                        "battery_percent": 76,
                        "days": "128",
                        "wind_mph": 4.2,
                    }
                    base.update(payloads[idx["i"]])
                    data = base
                    idx["i"] += 1
                else:
                    data = base  # type: ignore[name-defined]
                route.fulfill(status=200, content_type="application/json", body=json.dumps(data))

            page.route("**/sample_data.json", handle_route)

            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            _wait_sim_settled(page)

            # Capture OUT_TEMP rectangle pixels before refresh
            OUT_TEMP = [129, 36, 94, 28]  # Updated to match display_geometry.json
            js_read = (
                "([x,y,w,h])=>{"
                "const c=document.getElementById('epd');const ctx=c.getContext('2d');"
                "return Array.from(ctx.getImageData(x,y,w,h).data);}"
            )
            before = page.evaluate(js_read, OUT_TEMP)

            # Click Refresh → fetches sample_data.json and redraws with new
            # data. Wait for a draw stamped AFTER the click (the refresh
            # handler bumps __lastDrawAt), not just any earlier quiet period.
            before_draw = page.evaluate("() => window.__lastDrawAt || 0")
            page.click("#refresh")
            page.wait_for_function(
                "(prev) => window.__lastDrawAt && window.__lastDrawAt > prev",
                arg=before_draw,
            )

            after = page.evaluate(js_read, OUT_TEMP)

            # The OUT_TEMP area should change since the temperature changed from 68.4 to 100.0
            assert before != after, "Display should update when data changes"
            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)
