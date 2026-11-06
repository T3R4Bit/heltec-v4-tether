#!/usr/bin/env python3
"""Live TETHER dashboard: map, signal over time, signal vs distance, max distance, message history.

Reads the SQLite file the logger writes (both can run at once). Standard library only.

    python tools/dashboard.py --db rangetest.db                 # http://127.0.0.1:8080 on this laptop
    python tools/dashboard.py --db rangetest.db --host 0.0.0.0  # reachable from a phone on the same LAN/Tailscale

Phone GPS ("use this phone as my position") needs HTTPS on iOS. Easiest: `tailscale serve --bg 8080` and open the
https://<laptop>.<tailnet>.ts.net URL, or pass --cert/--key for your own certificate.
Anyone who can reach the port can read the data and post positions; keep it on 127.0.0.1, your LAN or your tailnet.
"""

import argparse
import json
import mimetypes
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tether_tools import analysis  # noqa: E402
from tether_tools.db import Store  # noqa: E402
from tether_tools.ingest import Ingestor  # noqa: E402

STATIC_DIR = Path(__file__).resolve().parent / "static"
mimetypes.add_type("application/manifest+json", ".webmanifest")
mimetypes.add_type("text/javascript", ".js")


class App:
    def __init__(self, store: Store):
        self.store = store
        self._geo = None  # Ingestor for phone positions, created on first use

    def geo_ingestor(self) -> Ingestor:
        if self._geo is None:
            session = self.store.new_session("dashboard-geo", local_node=analysis.local_node(self.store))
            self._geo = Ingestor(self.store, session, analysis.local_node(self.store))
        return self._geo


def make_handler(app: App):
    class Handler(BaseHTTPRequestHandler):
        server_version = "TetherDashboard/1"

        def log_message(self, fmt, *args):  # quieter than the default
            if self.path.startswith("/api/"):
                return
            sys.stderr.write("dashboard: %s\n" % (fmt % args))

        def _json(self, obj, status=200):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _static(self, rel: str):
            path = (STATIC_DIR / rel).resolve()
            if STATIC_DIR not in path.parents or not path.is_file():  # blocks ../ escapes
                self.send_error(404)
                return
            data = path.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", mimetypes.guess_type(str(path))[0] or "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            if path.name == "sw.js":
                self.send_header("Service-Worker-Allowed", "/")
                self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(url.query).items()}
            s = app.store
            try:
                if url.path == "/api/summary":
                    self._json(analysis.summary(s, q.get("peer")))
                elif url.path == "/api/signal":
                    peer = q.get("peer") or analysis.pick_peer(s, analysis.local_node(s))
                    self._json(analysis.signal_series(s, peer, float(q.get("since", 0))) if peer else {"rx": [], "rpt": []})
                elif url.path == "/api/distance":
                    local = analysis.local_node(s)
                    peer = q.get("peer") or analysis.pick_peer(s, local)
                    self._json(analysis.distance_series(s, local, peer) if peer else [])
                elif url.path == "/api/track":
                    local = analysis.local_node(s)
                    peer = q.get("peer") or analysis.pick_peer(s, local)
                    self._json({"peer": analysis.track(s, peer) if peer else [],
                                "local": analysis.track(s, local) if local else [],
                                "phone": analysis.track(s, "phone"), "base": analysis.base_position(s),
                                "peer_fixed": analysis.peer_position(s)})
                elif url.path == "/api/messages":
                    self._json(analysis.messages(s, int(q.get("limit", 100))))
                elif url.path in ("/", "/index.html"):
                    self._static("index.html")
                else:
                    self._static(url.path.lstrip("/"))
            except (ValueError, KeyError) as ex:
                self._json({"error": str(ex)}, 400)

        def do_POST(self):
            url = urlparse(self.path)
            if url.path != "/api/geo":
                self.send_error(404)
                return
            try:
                length = min(int(self.headers.get("Content-Length", 0)), 4096)
                body = json.loads(self.rfile.read(length) or b"{}")
                lat, lon = float(body["lat"]), float(body["lon"])
                if not (-90 <= lat <= 90 and -180 <= lon <= 180):
                    raise ValueError("lat/lon out of range")
                acc = body.get("accuracy")
                app.geo_ingestor().ingest_phone_position(lat, lon, float(acc) if acc is not None else None,
                                                         float(body.get("t", time.time())))
                self._json({"ok": True})
            except (ValueError, KeyError, TypeError) as ex:
                self._json({"error": str(ex)}, 400)

    return Handler


def serve(store: Store, host: str, port: int, cert=None, key=None) -> ThreadingHTTPServer:
    httpd = ThreadingHTTPServer((host, port), make_handler(App(store)))
    if cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    return httpd


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default="tether.db")
    ap.add_argument("--host", default="127.0.0.1", help="interface to listen on (0.0.0.0 for LAN)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--cert", help="TLS certificate (PEM), enables HTTPS")
    ap.add_argument("--key", help="TLS private key (PEM)")
    args = ap.parse_args()
    if not Path(args.db).exists():
        print("dashboard: %s does not exist yet; it will fill once the logger writes to it" % args.db)
    store = Store(args.db)
    httpd = serve(store, args.host, args.port, args.cert, args.key)
    print("dashboard: http%s://%s:%d  (db %s)" % ("s" if args.cert else "", args.host, httpd.server_address[1], args.db))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        store.close()


if __name__ == "__main__":
    main()
