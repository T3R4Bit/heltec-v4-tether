"""Turns Meshtastic packet dicts and device log lines into database rows.

Live logging and replay both go through Ingestor, so replaying a dataset exercises the same code as a real node.
"""

import time
from typing import Optional

from .db import Store
from .parse import node_id, parse_tether_line, position_from_decoded

BROADCAST_NUM = 0xFFFFFFFF


def _to_id(packet: dict) -> Optional[str]:
    to = packet.get("to")
    if to is None:
        return packet.get("toId")
    if int(to) & 0xFFFFFFFF == BROADCAST_NUM:
        return "^all"
    return node_id(to)


class Ingestor:
    def __init__(self, store: Store, session: int, local_node: Optional[str] = None):
        self.store = store
        self.session = session
        self.local_node = local_node
        self.counts = {"packets": 0, "positions": 0, "texts": 0, "tether": 0}
        self._last_tether_fix = None

    def set_local_node(self, local_node: str) -> None:
        self.local_node = local_node
        self.store.set_local_node(self.session, local_node)

    def ingest_packet(self, packet: dict, t: Optional[float] = None) -> None:
        """packet: dict as published by the meshtastic library on 'meshtastic.receive' (camelCase keys)."""
        t = time.time() if t is None else t
        from_num = packet.get("from")
        from_id = node_id(from_num) if from_num is not None else packet.get("fromId")
        decoded = packet.get("decoded") or {}
        portnum = decoded.get("portnum") or ("ENCRYPTED" if "encrypted" in packet else None)
        text = decoded.get("text") if portnum == "TEXT_MESSAGE_APP" else None

        # The protobuf -> dict conversion drops zero-valued fields, so a missing hop field means 0.
        self.store.add_packet(
            self.session,
            t,
            packet_id=packet.get("id"),
            from_id=from_id,
            to_id=_to_id(packet),
            portnum=portnum,
            channel=packet.get("channel", 0),
            rssi=packet.get("rxRssi"),
            snr=packet.get("rxSnr"),
            hop_start=packet.get("hopStart", 0),
            hop_limit=packet.get("hopLimit", 0),
            via_mqtt=1 if packet.get("viaMqtt") else 0,
            text=text,
        )
        self.counts["packets"] += 1
        if text is not None:
            self.counts["texts"] += 1

        if portnum == "POSITION_APP" and from_id:
            pos = position_from_decoded(decoded.get("position") or {})
            if pos:
                self.store.add_position(self.session, t, from_id, pos["lat"], pos["lon"], "mesh", alt=pos["alt"],
                                        sats=pos["sats"], dop=pos["dop"])
                self.counts["positions"] += 1

    def ingest_log_line(self, line: str, t: Optional[float] = None) -> bool:
        """Store a TETHER line printed by the attached node. Other log lines are ignored. Returns True if stored."""
        parsed = parse_tether_line(line)
        if not parsed:
            return False
        t = time.time() if t is None else t
        if parsed.get("event") == "boot" and isinstance(parsed.get("node"), str) and not self.local_node:
            self.set_local_node(parsed["node"])  # firmware prints its own node id at boot
        self.store.add_tether(self.session, t, self.local_node, parsed)
        self.counts["tether"] += 1

        # GPS-tagged samples (firmware TETHER_GPS_TAG): the attached node's own smoothed fix, already quality-gated.
        if parsed.get("event") in ("rx", "rpt") and parsed.get("gps") in ("ok", "fixed"):
            lat, lon = parsed.get("lat"), parsed.get("lon")
            if isinstance(lat, float) and isinstance(lon, float) and (lat, lon) != self._last_tether_fix:
                self.store.add_position(self.session, t, self.local_node or "local", lat, lon, "tether-gps",
                                        alt=parsed.get("alt"), sats=parsed.get("sats"), dop=parsed.get("hdop"))
                self._last_tether_fix = (lat, lon)
                self.counts["positions"] += 1
        return True

    def ingest_phone_position(self, lat: float, lon: float, accuracy_m: Optional[float] = None,
                              t: Optional[float] = None) -> None:
        """A position from the dashboard's browser geolocation, standing in for the attached node's own GPS."""
        node = self.local_node or "phone"
        self.store.add_position(self.session, time.time() if t is None else t, node, lat, lon, "phone",
                                accuracy_m=accuracy_m)
        self.counts["positions"] += 1
