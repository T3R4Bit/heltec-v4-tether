"""Parsing helpers: TETHER log lines, node ids and geo math."""

import math
import re
from typing import Dict, Optional

# Firmware emits e.g. "TETHER rx kind=reply peer=!1a2b3c4d seq=42 rssi=-91 snr=6.25 ..." (docs/DESIGN_NOTES.md 7).
# Meshtastic prefixes log lines ("INFO  | 12:00:01 123 [Tether] "), so the match can start anywhere in the line.
_TETHER_RE = re.compile(r"\bTETHER\s+([a-z_]+)((?:\s+[a-z_0-9]+=\S+)*)\s*$")
_KV_RE = re.compile(r"([a-z_0-9]+)=(\S+)")

_INT_FIELDS = {"seq", "rssi", "port", "sent", "replies", "heard", "replied", "missed", "restarts", "skipped", "passive",
               "window", "backoff", "interval_ms", "airtime_ms", "missed_total", "version", "budget_pct",
               "min_interval_ms", "code", "result", "hop_start", "hop_limit", "sats", "fix", "gps_t", "alt",
               "vbat_mv", "usb", "charging", "mult", "power"}


def _coerce(key: str, value: str):
    if value == "na":
        return None
    if key in _INT_FIELDS:
        try:
            return int(value)
        except ValueError:
            return value
    try:
        return float(value)
    except ValueError:
        return value


def parse_tether_line(line: str) -> Optional[Dict]:
    """Parse one TETHER log line into {"event": ..., <fields>}. Returns None for anything else."""
    if not line or "TETHER" not in line:
        return None
    m = _TETHER_RE.search(line.strip())
    if not m:
        return None
    out = {"event": m.group(1)}
    for k, v in _KV_RE.findall(m.group(2)):
        out[k] = _coerce(k, v)
    return out


def node_id(num: Optional[int]) -> Optional[str]:
    """Meshtastic node number -> '!xxxxxxxx'."""
    if num is None:
        return None
    return "!%08x" % (int(num) & 0xFFFFFFFF)


def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in metres."""
    r = 6371008.8
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(a)))


def position_from_decoded(pos: Dict) -> Optional[Dict]:
    """Normalise a Meshtastic position dict (from the Python library) to lat/lon/alt/sats/pdop."""
    if not pos:
        return None
    lat = pos.get("latitude")
    lon = pos.get("longitude")
    if lat is None and "latitudeI" in pos:
        lat = pos["latitudeI"] * 1e-7
    if lon is None and "longitudeI" in pos:
        lon = pos["longitudeI"] * 1e-7
    if lat is None or lon is None or (lat == 0 and lon == 0):
        return None
    pdop = pos.get("PDOP")
    return {
        "lat": float(lat),
        "lon": float(lon),
        "alt": pos.get("altitude"),
        "sats": pos.get("satsInView"),
        "dop": (pdop / 100.0) if isinstance(pdop, (int, float)) else None,  # firmware sends DOP x100
    }
