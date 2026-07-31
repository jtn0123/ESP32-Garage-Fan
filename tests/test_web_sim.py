import contextlib
import os
import socket
import subprocess
import sys
import time

import pytest


def _find_free_port() -> int:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _start_http_server(root: str, port: int) -> subprocess.Popen:
    return subprocess.Popen(
        [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"],
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


@pytest.mark.skipif(
    not bool(__import__("importlib").util.find_spec("playwright")),
    reason="playwright not installed",
)
def test_web_sim_basic_pixels():
    from playwright.sync_api import sync_playwright  # type: ignore

    web_root = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web")
    port = _find_free_port()
    server = _start_http_server(web_root, port)
    try:
        time.sleep(0.4)
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 250, "height": 122})
            page.goto(f"http://127.0.0.1:{port}/sim/index.html", wait_until="load")
            _wait_sim_settled(page)

            # Corners should be black border
            for x, y in [(0, 0), (249, 0), (0, 121), (249, 121)]:
                r, g, b, a = _canvas_rgba(page, x, y)
                assert (r, g, b) == (0, 0, 0)

            # v3 header is an inverted band (fill op): solid black inside the
            # band; the old y=14 underline is deliberately gone.
            r, g, b, a = _canvas_rgba(page, 5, 7)
            assert (r, g, b) == (0, 0, 0)
            r, g, b, a = _canvas_rgba(page, 5, 14)
            assert (r, g, b) == (255, 255, 255)

            # Footer separator: read its y from the spec rather than hardcoding.
            # It previously lived at y=80 and the spec moved it to 84, which this
            # assertion silently outlived because the job could not fail.
            sep_y = page.evaluate("""() => {
                  const ops = (window.UI_SPEC || {}).components?.chrome || [];
                  const h = ops.filter(o => o.op === 'line'
                    && o.from[1] === o.to[1]
                    && Math.abs(o.to[0] - o.from[0]) > 200
                    && o.from[1] > 14 && o.from[1] < 121);
                  return h.length ? h[0].from[1] : null;
                }""")
            assert sep_y is not None, "no footer separator line found in ui_spec chrome"
            r, g, b, a = _canvas_rgba(page, 5, sep_y)
            assert (r, g, b) == (0, 0, 0), f"footer separator missing at y={sep_y}"

            # Bottom-right weather area (split3) should contain some non-white pixels
            any_black = False
            for y in range(95, 120):
                for x in range(130, 244):
                    r, g, b, a = _canvas_rgba(page, x, y)
                    if (r, g, b) != (255, 255, 255):
                        any_black = True
                        break
                if any_black:
                    break
            assert any_black

            browser.close()
    finally:
        server.terminate()
        server.wait(timeout=2)
