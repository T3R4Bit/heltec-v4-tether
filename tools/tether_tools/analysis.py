"""Queries behind the dashboard: who is heard, signal over time, signal vs distance, max distance."""

import bisect
import json
from typing import Dict, List, Optional, Tuple

from .db import Store
from .parse import haversine_m

POSITION_WINDOW_S = 120.0  # a position counts for a sample if it is within this many seconds of it


def is_synthetic(store: Store) -> bool:
    return bool(store.query("SELECT 1 FROM sessions WHERE synthetic=1 LIMIT 1"))


def local_node(store: Store) -> Optional[str]:
    rows = store.query("SELECT local_node FROM sessions WHERE local_node IS NOT NULL ORDER BY id DESC LIMIT 1")
    return rows[0]["local_node"] if rows else None


def base_position(store: Store) -> Optional[Tuple[float, float]]:
    rows = {r["key"]: r["value"] for r in store.query("SELECT key, value FROM meta WHERE key IN ('base_lat','base_lon')")}
    if "base_lat" in rows and "base_lon" in rows:
        return float(rows["base_lat"]), float(rows["base_lon"])
    return None


def set_base_position(store: Store, lat: float, lon: float) -> None:
    with store._lock:
        store.conn.executemany("INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)",
                               [("base_lat", str(lat)), ("base_lon", str(lon))])
        store.conn.commit()


def nodes(store: Store, local: Optional[str]) -> List[Dict]:
    """Remote nodes heard directly over LoRa, busiest first."""
    rows = store.query(
        "SELECT from_id AS id, COUNT(*) AS packets, MAX(t) AS last_t FROM packets "
        "WHERE from_id IS NOT NULL AND (? IS NULL OR from_id != ?) AND via_mqtt=0 AND rssi IS NOT NULL "
        "AND hop_start = hop_limit GROUP BY from_id ORDER BY packets DESC",
        (local, local),
    )
    for r in rows:
        last = store.query("SELECT rssi, snr FROM packets WHERE from_id=? AND rssi IS NOT NULL ORDER BY t DESC LIMIT 1",
                           (r["id"],))
        r["last_rssi"] = last[0]["rssi"] if last else None
        r["last_snr"] = last[0]["snr"] if last else None
    return rows


def pick_peer(store: Store, local: Optional[str]) -> Optional[str]:
    """The TETHER peer if the node logged one, else the busiest directly heard node."""
    rows = store.query("SELECT peer FROM tether WHERE peer IS NOT NULL AND peer != 'none' ORDER BY t DESC LIMIT 1")
    if rows:
        return rows[0]["peer"]
    ns = nodes(store, local)
    return ns[0]["id"] if ns else None


def signal_series(store: Store, peer: str, since: float = 0.0) -> Dict[str, List[Dict]]:
    """rx: how the attached node heard the peer (every direct packet). rpt: how the peer heard us (TETHER reports)."""
    rx = store.query(
        "SELECT t, rssi, snr, portnum FROM packets WHERE from_id=? AND rssi IS NOT NULL AND via_mqtt=0 "
        "AND hop_start = hop_limit AND t > ? ORDER BY t",
        (peer, since),
    )
    rpt = store.query(
        "SELECT t, rssi, snr, seq FROM tether WHERE event='rpt' AND peer=? AND t > ? ORDER BY t", (peer, since)
    )
    return {"rx": rx, "rpt": rpt}


class _Track:
    """Time-sorted positions of one node with nearest-in-time lookup."""

    def __init__(self, rows: List[Dict]):
        self.ts = [r["t"] for r in rows]
        self.rows = rows

    def nearest(self, t: float, window: float) -> Optional[Dict]:
        if not self.ts:
            return None
        i = bisect.bisect_left(self.ts, t)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(self.ts):
                dt = abs(self.ts[j] - t)
                if dt <= window and (best is None or dt < best[0]):
                    best = (dt, self.rows[j])
        return best[1] if best else None


def track(store: Store, node: str) -> List[Dict]:
    return store.query("SELECT t, lat, lon, alt, sats, dop, accuracy_m, source FROM positions WHERE node_id=? ORDER BY t",
                       (node,))


def distance_series(store: Store, local: Optional[str], peer: str, window: float = POSITION_WINDOW_S) -> List[Dict]:
    """Each received sample from the peer paired with the distance between the two nodes at that moment.

    The local position comes from the attached node's GPS, the dashboard phone's geolocation, or a fixed base
    position (meta base_lat/base_lon), in that order. Samples without both positions are skipped.
    """
    peer_track = _Track(track(store, peer))
    local_track = _Track(track(store, local)) if local else _Track([])
    phone_track = _Track(track(store, "phone"))
    base = base_position(store)
    out = []
    for s in signal_series(store, peer)["rx"]:
        p = peer_track.nearest(s["t"], window)
        if not p:
            continue
        l = local_track.nearest(s["t"], window) or phone_track.nearest(s["t"], window)
        if l:
            here = (l["lat"], l["lon"])
        elif base:
            here = base
        else:
            continue
        out.append({"t": s["t"], "d_m": haversine_m(here[0], here[1], p["lat"], p["lon"]), "rssi": s["rssi"],
                    "snr": s["snr"]})
    return out


def max_distance(series: List[Dict]) -> Optional[Dict]:
    return max(series, key=lambda r: r["d_m"]) if series else None


def messages(store: Store, limit: int = 100) -> List[Dict]:
    rx = store.query(
        "SELECT t, from_id, to_id, text, rssi, snr, hop_start, hop_limit, 'rx' AS dir FROM packets WHERE text IS NOT NULL "
        "ORDER BY t DESC LIMIT ?", (limit,))
    tx = store.query("SELECT t, NULL AS from_id, to_id, text, NULL AS rssi, NULL AS snr, NULL AS hop_start, "
                     "NULL AS hop_limit, 'tx' AS dir FROM sent ORDER BY t DESC LIMIT ?", (limit,))
    return sorted(rx + tx, key=lambda r: r["t"], reverse=True)[:limit]


def latest_stats(store: Store) -> Optional[Dict]:
    rows = store.query("SELECT t, fields FROM tether WHERE event='stats' ORDER BY t DESC LIMIT 1")
    if not rows:
        return None
    out = json.loads(rows[0]["fields"])
    out["t"] = rows[0]["t"]
    return out


def summary(store: Store, peer: Optional[str] = None) -> Dict:
    local = local_node(store)
    peer = peer or pick_peer(store, local)
    dist = distance_series(store, local, peer) if peer else []
    best = max_distance(dist)
    counts = store.query("SELECT (SELECT COUNT(*) FROM packets) AS packets, (SELECT COUNT(*) FROM positions) AS positions, "
                         "(SELECT COUNT(*) FROM tether) AS tether, (SELECT COUNT(*) FROM sessions) AS sessions")[0]
    last = None
    if peer:
        rows = store.query("SELECT t, rssi, snr FROM packets WHERE from_id=? AND rssi IS NOT NULL AND via_mqtt=0 "
                           "AND hop_start = hop_limit ORDER BY t DESC LIMIT 1", (peer,))
        last = rows[0] if rows else None
    return {
        "synthetic": is_synthetic(store),
        "local_node": local,
        "peer": peer,
        "nodes": nodes(store, local),
        "max_distance": best,
        "last_rx": last,
        "last_distance": dist[-1] if dist else None,
        "tether_stats": latest_stats(store),
        "counts": counts,
        "base": base_position(store),
    }
