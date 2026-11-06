#!/usr/bin/env python3
"""End-to-end check of the dashboard in a real browser, using SYNTHETIC data.

Replays tools/synth.py output into a temp database, serves the dashboard, opens it in headless Chromium/Edge via
Playwright at phone and desktop sizes, fails on any JavaScript error, checks that every panel rendered, and writes
screenshots. Not part of the unit suite because it needs Playwright and a browser:

    pip install playwright          # then either `playwright install chromium` or use an installed Edge/Chrome
    python tools/tests/e2e_dashboard.py [--channel msedge] [--shots docs/screenshots]
"""

import argparse
import json
import os
import sys
import tempfile
import threading
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

import dashboard  # noqa: E402
import logger  # noqa: E402
import synth  # noqa: E402
from tether_tools.db import Store  # noqa: E402


def check_page(page, url, errors):
    page.goto(url)
    page.wait_for_function("document.querySelector('#t-max').textContent.trim() !== '–'", timeout=15000)
    page.wait_for_selector("#chart-time svg path.s1", timeout=15000)
    page.wait_for_selector("#chart-dist svg rect", timeout=15000)
    problems = []
    if not page.is_visible("#synthetic"):
        problems.append("synthetic banner not visible")
    if page.is_visible("#offline"):
        problems.append("offline banner visible")
    if page.locator("#msgs li .who").count() == 0:
        problems.append("no messages rendered")
    if page.locator("#map .leaflet-interactive").count() < 2:
        problems.append("map has no track/markers")
    if page.evaluate("document.documentElement.scrollWidth > window.innerWidth + 1"):
        problems.append("horizontal page scroll")
    tiles = {sel: page.inner_text(sel) for sel in ("#t-rssi", "#t-rpt", "#t-dist", "#t-max", "#t-pdr", "#t-pkts")}
    return problems + errors, tiles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default=os.environ.get("PW_CHANNEL", "msedge"),
                    help="browser channel: msedge, chrome, or '' for Playwright's bundled Chromium")
    ap.add_argument("--shots", default=None, help="directory for screenshots (default: none)")
    args = ap.parse_args()

    from playwright.sync_api import sync_playwright

    tmp = tempfile.TemporaryDirectory()
    db = os.path.join(tmp.name, "e2e.db")
    data = os.path.join(tmp.name, "synthetic.jsonl")
    with open(data, "w", encoding="utf-8") as fh:
        for e in synth.generate(seed=7, minutes=50):
            fh.write(json.dumps(e) + "\n")
    store = Store(db)
    logger.replay(store, data, speed=0)
    httpd = dashboard.serve(store, "127.0.0.1", 0)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % httpd.server_address[1]

    failed = False
    with sync_playwright() as pw:
        browser = pw.chromium.launch(channel=args.channel or None, headless=True)
        for name, viewport, scheme in (("mobile", {"width": 390, "height": 844}, "dark"),
                                       ("desktop", {"width": 1280, "height": 900}, "light")):
            ctx = browser.new_context(viewport=viewport, color_scheme=scheme, device_scale_factor=2)
            page = ctx.new_page()
            errors = []
            page.on("pageerror", lambda e, errors=errors: errors.append("pageerror: %s" % e))
            page.on("console", lambda m, errors=errors: errors.append("console.%s: %s" % (m.type, m.text))
                    if m.type == "error" and "tile.openstreetmap.org" not in m.text else None)
            problems, tiles = check_page(page, url, errors)
            print("e2e %s (%s): tiles %s" % (name, scheme, tiles))
            if problems:
                failed = True
                for p in problems:
                    print("  PROBLEM: " + p)
            if args.shots:
                Path(args.shots).mkdir(parents=True, exist_ok=True)
                page.wait_for_timeout(1500)  # let map tiles arrive if there is internet
                out = Path(args.shots) / ("dashboard-%s.png" % name)
                page.screenshot(path=str(out), full_page=True)
                print("  screenshot: %s" % out)
            ctx.close()
        browser.close()
    httpd.shutdown()
    store.close()
    tmp.cleanup()
    print("e2e: FAIL" if failed else "e2e: OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
