"""Tests for the laptop tools. Standard library only: python -m unittest discover -s tools/tests -v"""

import json
import os
import re
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

import dashboard  # noqa: E402
import logger  # noqa: E402
import synth  # noqa: E402
from tether_tools import analysis, lineproto  # noqa: E402
from tether_tools.db import Store  # noqa: E402
from tether_tools.ingest import Ingestor  # noqa: E402
from tether_tools.parse import haversine_m, node_id, parse_tether_line, position_from_decoded  # noqa: E402


class TempDB(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.tmp.name, "t.db")
        self.store = Store(self.db_path)

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()


class ParseTests(unittest.TestCase):
    def test_tether_rx_line_with_log_prefix(self):
        p = parse_tether_line("INFO  | 12:00:01 123 [Tether] TETHER rx kind=reply peer=!1a2b3c4d seq=42 rssi=-91 "
                              "snr=6.25 margin_snr=23.75 margin_rssi=40.5")
        self.assertEqual(p["event"], "rx")
        self.assertEqual(p["kind"], "reply")
        self.assertEqual(p["peer"], "!1a2b3c4d")
        self.assertEqual(p["seq"], 42)
        self.assertIsInstance(p["seq"], int)
        self.assertEqual(p["rssi"], -91)
        self.assertAlmostEqual(p["snr"], 6.25)

    def test_stats_line_na_becomes_none(self):
        p = parse_tether_line("TETHER stats peer=!1a2b3c4d role=initiator sent=10 replies=9 pdr32=0.90 rssi_ewma=na")
        self.assertEqual(p["role"], "initiator")
        self.assertEqual(p["sent"], 10)
        self.assertIsNone(p["rssi_ewma"])

    def test_idle_line_without_fields(self):
        self.assertEqual(parse_tether_line("TETHER idle reason=no_peer"), {"event": "idle", "reason": "no_peer"})

    def test_non_tether_lines(self):
        self.assertIsNone(parse_tether_line("INFO | Lora RX (id=0x1234)"))
        self.assertIsNone(parse_tether_line(""))
        self.assertIsNone(parse_tether_line("TETHERING is fun"))

    def test_node_id(self):
        self.assertEqual(node_id(0x1A2B3C4D), "!1a2b3c4d")
        self.assertEqual(node_id(5), "!00000005")
        self.assertIsNone(node_id(None))

    def test_haversine(self):
        self.assertAlmostEqual(haversine_m(0, 0, 1, 0), 111195, delta=20)  # one degree of latitude
        self.assertEqual(haversine_m(39.7, -105.0, 39.7, -105.0), 0)

    def test_position_normalisation(self):
        p = position_from_decoded({"latitudeI": 397392000, "longitudeI": -1049903000, "satsInView": 9, "PDOP": 150})
        self.assertAlmostEqual(p["lat"], 39.7392)
        self.assertAlmostEqual(p["lon"], -104.9903)
        self.assertEqual(p["sats"], 9)
        self.assertAlmostEqual(p["dop"], 1.5)
        self.assertIsNone(position_from_decoded({"latitude": 0, "longitude": 0}))  # "no fix"
        self.assertIsNone(position_from_decoded({}))


class IngestTests(TempDB):
    def test_text_position_and_hop_defaults(self):
        s = self.store.new_session("live")
        ing = Ingestor(self.store, s, "!0a0b0c0d")
        ing.ingest_packet({"from": 0x1A2B3C4D, "to": 0xFFFFFFFF, "rxRssi": -90, "rxSnr": 5.5, "hopStart": 3,
                           "hopLimit": 3, "decoded": {"portnum": "TEXT_MESSAGE_APP", "text": "hi"}}, t=100.0)
        # The protobuf->dict conversion omits zero fields: no hopStart/hopLimit means both 0 (direct).
        ing.ingest_packet({"from": 0x1A2B3C4D, "to": 0x0A0B0C0D, "rxRssi": -95, "rxSnr": 1.0,
                           "decoded": {"portnum": "PRIVATE_APP"}}, t=101.0)
        ing.ingest_packet({"from": 0x1A2B3C4D, "to": 0xFFFFFFFF, "rxRssi": -99, "rxSnr": -3.0, "hopStart": 3,
                           "hopLimit": 3, "decoded": {"portnum": "POSITION_APP",
                                                      "position": {"latitude": 39.75, "longitude": -104.98}}}, t=102.0)
        ing.ingest_packet({"from": 0x22222222, "to": 0xFFFFFFFF, "rxRssi": -80, "rxSnr": 9.0, "hopStart": 3,
                           "hopLimit": 2, "decoded": {"portnum": "TEXT_MESSAGE_APP", "text": "relayed"}}, t=103.0)
        rows = self.store.query("SELECT * FROM packets ORDER BY t")
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0]["to_id"], "^all")
        self.assertEqual(rows[0]["text"], "hi")
        self.assertEqual((rows[1]["hop_start"], rows[1]["hop_limit"]), (0, 0))
        self.assertEqual(len(self.store.query("SELECT * FROM positions")), 1)
        # Relayed packet is stored but is not a direct signal sample.
        self.assertEqual([n["id"] for n in analysis.nodes(self.store, "!0a0b0c0d")], ["!1a2b3c4d"])
        self.assertEqual(len(analysis.signal_series(self.store, "!1a2b3c4d")["rx"]), 3)

    def test_gps_tagged_lines_become_positions(self):
        s = self.store.new_session("live")
        ing = Ingestor(self.store, s, "!0a0b0c0d")
        tag = " lat=39.739200 lon=-104.990300 alt=1609 hdop=0.90 sats=11 fix=3 gps_t=1762412431 gps=ok"
        ing.ingest_log_line("TETHER rx kind=reply peer=!1a2b3c4d seq=1 rssi=-91 snr=6.25" + tag, 10.0)
        ing.ingest_log_line("TETHER rpt kind=ping peer=!1a2b3c4d seq=1 rssi=-93 snr=5.0" + tag, 10.0)  # same fix
        ing.ingest_log_line("TETHER rx kind=reply peer=!1a2b3c4d seq=2 rssi=-91 snr=6.25 gps=hdop sats=7 hdop=3.10", 20.0)
        rows = self.store.query("SELECT * FROM positions")
        self.assertEqual(len(rows), 1)  # duplicate fix skipped, poor fix ignored
        self.assertEqual((rows[0]["node_id"], rows[0]["source"], rows[0]["sats"]), ("!0a0b0c0d", "tether-gps", 11))
        self.assertAlmostEqual(rows[0]["dop"], 0.9)
        p = json.loads(self.store.query("SELECT fields FROM tether WHERE seq=2")[0]["fields"])
        self.assertEqual((p["gps"], p["sats"]), ("hdop", 7))

    def test_power_line_in_summary(self):
        s = self.store.new_session("live")
        ing = Ingestor(self.store, s, "!0a0b0c0d")
        ing.ingest_log_line("TETHER power vbat_mv=3950 vbat_ewma=3948 pct=81.5 usb=0 charging=0 trend_pct_h=-6.00 "
                            "runtime_h=13.6 mult=1", 30.0)
        pw = analysis.summary(self.store)["power"]
        self.assertEqual(pw["vbat_mv"], 3950)
        self.assertAlmostEqual(pw["pct"], 81.5)
        self.assertAlmostEqual(pw["runtime_h"], 13.6)
        self.assertEqual(pw["charging"], 0)

    def test_log_lines(self):
        s = self.store.new_session("live")
        ing = Ingestor(self.store, s, "!0a0b0c0d")
        self.assertTrue(ing.ingest_log_line("[Tether] TETHER rpt kind=ping peer=!1a2b3c4d seq=3 rssi=-101 snr=-2.5", 5.0))
        self.assertFalse(ing.ingest_log_line("DEBUG | something else", 6.0))
        row = self.store.query("SELECT * FROM tether")[0]
        self.assertEqual((row["event"], row["kind"], row["seq"], row["rssi"]), ("rpt", "ping", 3, -101))
        self.assertEqual(row["node_id"], "!0a0b0c0d")
        self.assertEqual(json.loads(row["fields"])["peer"], "!1a2b3c4d")


class SyntheticReplayTests(TempDB):
    @classmethod
    def setUpClass(cls):
        cls.events = synth.generate(seed=3, minutes=30)

    def replay(self):
        path = os.path.join(self.tmp.name, "s.jsonl")
        with open(path, "w", encoding="utf-8") as fh:
            for e in self.events:
                fh.write(json.dumps(e) + "\n")
        return logger.replay(self.store, path, speed=0)

    def test_header_marks_synthetic(self):
        self.assertTrue(self.events[0]["synthetic"])
        self.assertIn("SYNTHETIC", self.events[0]["note"])

    def test_deterministic(self):
        self.assertEqual(synth.generate(seed=3, minutes=30), self.events)
        self.assertNotEqual(synth.generate(seed=4, minutes=30), self.events)

    def test_replay_fills_db_and_summary(self):
        ing = self.replay()
        self.assertGreater(ing.counts["packets"], 50)
        self.assertGreater(ing.counts["tether"], 100)
        s = analysis.summary(self.store)
        self.assertTrue(s["synthetic"])
        self.assertEqual(s["local_node"], "!0a0b0c0d")
        self.assertEqual(s["peer"], "!1a2b3c4d")
        self.assertIsNotNone(s["max_distance"])
        # Walking at 1.4 m/s for 70 % of 30 min, the peer is never further than about 2.5 km.
        self.assertGreater(s["max_distance"]["d_m"], 500)
        self.assertLess(s["max_distance"]["d_m"], 1.4 * 30 * 60 * 0.7 + 50)
        self.assertEqual(s["tether_stats"]["role"], "initiator")

    def test_signal_falls_with_distance(self):
        self.replay()
        dist = analysis.distance_series(self.store, "!0a0b0c0d", "!1a2b3c4d")
        near = [d["snr"] for d in dist if d["d_m"] < 300]
        far = [d["snr"] for d in dist if d["d_m"] > 1500]
        self.assertTrue(near and far)
        self.assertGreater(sum(near) / len(near), sum(far) / len(far) + 5)
        sig = analysis.signal_series(self.store, "!1a2b3c4d")
        self.assertTrue(sig["rx"] and sig["rpt"])

    def test_base_position_fallback(self):
        s = self.store.new_session("live")
        ing = Ingestor(self.store, s)  # attached node unknown, no own GPS
        ing.ingest_packet({"from": 0x1A2B3C4D, "rxRssi": -90, "rxSnr": 1.0, "decoded": {
            "portnum": "POSITION_APP", "position": {"latitude": 39.75, "longitude": -104.99}}}, t=10.0)
        self.assertEqual(analysis.distance_series(self.store, None, "!1a2b3c4d"), [])
        analysis.set_base_position(self.store, 39.74, -104.99)
        d = analysis.distance_series(self.store, None, "!1a2b3c4d")
        self.assertEqual(len(d), 1)
        self.assertAlmostEqual(d[0]["d_m"], 1112, delta=5)


class LineProtocolTests(TempDB):
    def test_line_format_and_escaping(self):
        ln = lineproto.line("tether_rx", {"peer": "!1a", "kind": "re ply", "empty": None}, {"rssi": -91, "snr": 6.25,
                                                                                            "note": 'a"b'}, 1.5)
        self.assertEqual(ln, 'tether_rx,kind=re\\ ply,peer=!1a note="a\\"b",rssi=-91i,snr=6.25 1500000000')
        self.assertIsNone(lineproto.line("m", {}, {"x": None}, 0))

    def test_export(self):
        s = self.store.new_session("live")
        ing = Ingestor(self.store, s, "!0a0b0c0d")
        ing.ingest_log_line("TETHER rx kind=reply peer=!1a2b3c4d seq=1 rssi=-91 snr=6.25", 10.0)
        ing.ingest_packet({"from": 0x1A2B3C4D, "rxRssi": -91, "rxSnr": 6.25, "decoded": {"portnum": "PRIVATE_APP"}}, 10.0)
        lines = list(lineproto.export(self.store))
        self.assertEqual(len(lines), 2)
        pat = re.compile(r"^[a-z_]+(,[^ ]+)? [^ ]+ \d+$")
        for ln in lines:
            self.assertRegex(ln, pat)
        self.assertTrue(lines[0].startswith("tether_rx,kind=reply,node=!0a0b0c0d,peer=!1a2b3c4d "))
        self.assertIn("seq=1i", lines[0])


class DashboardTests(TempDB):
    def setUp(self):
        super().setUp()
        path = os.path.join(self.tmp.name, "s.jsonl")
        with open(path, "w", encoding="utf-8") as fh:
            for e in synth.generate(seed=5, minutes=20):
                fh.write(json.dumps(e) + "\n")
        logger.replay(self.store, path, speed=0)
        self.httpd = dashboard.serve(self.store, "127.0.0.1", 0)
        self.base = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        super().tearDown()

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as r:
            return r.status, r.headers.get("Content-Type"), r.read()

    def post(self, path, obj):
        req = urllib.request.Request(self.base + path, data=json.dumps(obj).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_api_endpoints(self):
        _, ctype, body = self.get("/api/summary")
        self.assertEqual(ctype, "application/json")
        s = json.loads(body)
        self.assertTrue(s["synthetic"])
        self.assertEqual(s["peer"], "!1a2b3c4d")
        sig = json.loads(self.get("/api/signal")[2])
        self.assertTrue(sig["rx"])
        dist = json.loads(self.get("/api/distance?peer=!1a2b3c4d")[2])
        self.assertTrue(dist and all("d_m" in d for d in dist))
        trk = json.loads(self.get("/api/track")[2])
        self.assertTrue(trk["peer"] and trk["local"])
        msgs = json.loads(self.get("/api/messages?limit=5")[2])
        self.assertLessEqual(len(msgs), 5)
        self.assertTrue(all(m["text"].startswith("checkpoint") for m in msgs))

    def test_static_and_pwa_files(self):
        status, ctype, body = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", ctype)
        self.assertIn(b"manifest.webmanifest", body)
        _, ctype, body = self.get("/manifest.webmanifest")
        self.assertEqual(ctype, "application/manifest+json")
        manifest = json.loads(body)
        for icon in manifest["icons"]:
            st, ct, data = self.get("/" + icon["src"])
            self.assertEqual((st, ct), (200, "image/png"))
            self.assertTrue(data.startswith(b"\x89PNG"))
        self.assertEqual(self.get("/sw.js")[0], 200)
        self.assertEqual(self.get("/vendor/leaflet-1.9.4/leaflet.js")[0], 200)

    def test_no_path_traversal(self):
        for path in ("/../dashboard.py", "/%2e%2e/dashboard.py", "/vendor/../../logger.py", "/nope.html"):
            with self.assertRaises(urllib.error.HTTPError) as cm:
                self.get(path)
            self.assertEqual(cm.exception.code, 404)

    def test_phone_geolocation(self):
        status, body = self.post("/api/geo", {"lat": 39.74, "lon": -104.99, "accuracy": 8, "t": 123.0})
        self.assertEqual((status, body), (200, {"ok": True}))
        rows = self.store.query("SELECT * FROM positions WHERE source='phone'")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["node_id"], "!0a0b0c0d")  # stands in for the attached node
        self.assertEqual(rows[0]["accuracy_m"], 8)
        self.assertEqual(self.post("/api/geo", {"lat": 100, "lon": 0})[0], 400)
        self.assertEqual(self.post("/api/geo", {"lat": "x"})[0], 400)
        self.assertEqual(self.post("/api/geo", {})[0], 400)


if __name__ == "__main__":
    unittest.main()
