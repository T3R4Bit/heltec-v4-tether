#!/usr/bin/env python3
"""Record everything a Meshtastic node hears into SQLite, and send messages from the terminal.

Live (needs `pip install meshtastic`):
    python tools/logger.py --port COM5 --db rangetest.db        # USB serial (Linux/macOS: /dev/ttyUSB0 etc.)
    python tools/logger.py --host 192.168.1.50 --db rangetest.db # node with WiFi/TCP API
  Type a line and press Enter to broadcast it. `/dm !1a2b3c4d hello` sends a DM. `/quit` exits.
  TETHER log lines reach the logger only if the node forwards its debug log to the API client:
    meshtastic --set security.debug_log_api_enabled true

Replay (no hardware):
    python tools/synth.py --out synth.jsonl
    python tools/logger.py --replay synth.jsonl --db demo.db             # instant
    python tools/logger.py --replay synth.jsonl --db demo.db --speed 20  # 20x real time, for watching the dashboard

Export to InfluxDB line protocol (self-hosted InfluxDB/Grafana):
    python tools/logger.py --db rangetest.db --export-lp rangetest.lp
"""

import argparse
import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tether_tools import lineproto  # noqa: E402
from tether_tools.db import Store  # noqa: E402
from tether_tools.ingest import Ingestor  # noqa: E402
from tether_tools.parse import node_id, position_from_decoded  # noqa: E402


def describe(packet: dict) -> str:
    d = packet.get("decoded") or {}
    port = d.get("portnum", "ENCRYPTED")
    sig = ""
    if packet.get("rxRssi") is not None:
        sig = " rssi=%s snr=%s" % (packet.get("rxRssi"), packet.get("rxSnr"))
    hops = packet.get("hopStart", 0) - packet.get("hopLimit", 0)
    text = (' "%s"' % d["text"]) if "text" in d else ""
    return "%s -> %s %s%s hops=%d%s" % (node_id(packet.get("from")), packet.get("toId", packet.get("to")), port, sig, hops,
                                         text)


def replay(store: Store, path: str, speed: float) -> Ingestor:
    """Feed a JSONL dataset through the normal ingestion path. speed 0 keeps original timestamps and runs instantly;
    speed > 0 replays in (scaled) real time with timestamps rebased to now."""
    with open(path, encoding="utf-8") as fh:
        events = [json.loads(line) for line in fh if line.strip()]
    header = events[0] if events and events[0].get("type") == "header" else {}
    synthetic = bool(header.get("synthetic"))
    session = store.new_session("synthetic" if synthetic else "replay", synthetic=synthetic,
                                note="%s (%s)" % (header.get("note", "replay"), Path(path).name))
    ing = Ingestor(store, session)
    if synthetic:
        print("logger: replaying SYNTHETIC data from %s" % path)

    timed = [e for e in events if "t" in e]
    t0 = timed[0]["t"] if timed else 0.0
    wall0 = time.time()
    for e in events:
        kind = e.get("type")
        if speed > 0 and "t" in e:
            due = wall0 + (e["t"] - t0) / speed
            delay = due - time.time()
            if delay > 0:
                time.sleep(delay)
        t = time.time() if speed > 0 else e.get("t")
        if kind == "local_node":
            ing.set_local_node(e["id"])
        elif kind == "packet":
            ing.ingest_packet(e["packet"], t)
            if speed > 0:
                print("rx " + describe(e["packet"]))
        elif kind == "log":
            ing.ingest_log_line(e["line"], t)
    print("logger: replay done: %s" % ing.counts)
    return ing


def live(store: Store, args) -> None:
    try:
        from pubsub import pub
        import meshtastic.serial_interface
        import meshtastic.tcp_interface
    except ImportError:
        sys.exit("logger: live mode needs the meshtastic package: pip install -r tools/requirements.txt")

    session = store.new_session("live")
    ing = Ingestor(store, session)

    def on_receive(packet, interface):  # noqa: ARG001 (pubsub passes interface)
        try:
            ing.ingest_packet(packet)
            print("rx " + describe(packet))
        except Exception as ex:  # keep logging even if one packet is odd
            print("logger: failed to store packet: %s" % ex, file=sys.stderr)

    def on_log(line, interface):  # noqa: ARG001
        if ing.ingest_log_line(line):
            print("log " + line[line.find("TETHER"):])

    pub.subscribe(on_receive, "meshtastic.receive")
    pub.subscribe(on_log, "meshtastic.log.line")

    if args.host:
        iface = meshtastic.tcp_interface.TCPInterface(hostname=args.host)
    else:
        iface = meshtastic.serial_interface.SerialInterface(devPath=args.port)

    info = iface.getMyNodeInfo() or {}
    me = node_id(info.get("num"))
    if me:
        ing.set_local_node(me)
    print("logger: connected as %s, writing to %s (session %d). Type to send, /dm !id text, /quit" % (me, args.db, session))

    stop = threading.Event()

    def poll_own_position():
        last = None
        while not stop.is_set():
            pos = position_from_decoded(((iface.getMyNodeInfo() or {}).get("position")) or {})
            if pos and me and (pos["lat"], pos["lon"]) != last:
                store.add_position(session, time.time(), me, pos["lat"], pos["lon"], "mesh", alt=pos["alt"],
                                   sats=pos["sats"], dop=pos["dop"])
                last = (pos["lat"], pos["lon"])
            stop.wait(15)

    threading.Thread(target=poll_own_position, daemon=True).start()

    try:
        for raw in sys.stdin:
            line = raw.strip()
            if not line:
                continue
            if line == "/quit":
                break
            dest = "^all"
            if line.startswith("/dm "):
                parts = line.split(" ", 2)
                if len(parts) < 3:
                    print("usage: /dm !1a2b3c4d message")
                    continue
                dest, line = parts[1], parts[2]
            iface.sendText(line, destinationId=dest)
            store.add_sent(session, time.time(), dest, line)
            print("tx -> %s: %s" % (dest, line))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        iface.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default="tether.db", help="SQLite file (default tether.db)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--port", help="serial port of the node, e.g. COM5 or /dev/ttyUSB0 (default: auto-detect)")
    src.add_argument("--host", help="hostname/IP of a node with the TCP API enabled")
    src.add_argument("--replay", help="JSONL dataset to replay instead of connecting to a node")
    src.add_argument("--export-lp", metavar="FILE", help="write the database as InfluxDB line protocol and exit")
    ap.add_argument("--speed", type=float, default=0.0, help="replay speed multiplier (0 = instant)")
    ap.add_argument("--base", metavar="LAT,LON", help="fixed position of the attached node, if it has no GPS")
    args = ap.parse_args()

    store = Store(args.db)
    if args.base:
        from tether_tools.analysis import set_base_position
        lat, lon = (float(x) for x in args.base.split(","))
        set_base_position(store, lat, lon)
    try:
        if args.export_lp:
            n = lineproto.write_file(lineproto.export(store), args.export_lp)
            print("logger: wrote %d lines to %s" % (n, args.export_lp))
        elif args.replay:
            replay(store, args.replay, args.speed)
        else:
            live(store, args)
    finally:
        store.close()


if __name__ == "__main__":
    main()
