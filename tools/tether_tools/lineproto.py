"""InfluxDB line protocol export (Phase 4), for a self-hosted InfluxDB/Grafana.

Each TETHER rx/rpt event and each directly received packet becomes one line, e.g.
    tether_rx,node=!0a0b0c0d,peer=!1a2b3c4d,kind=reply rssi=-91i,snr=6.25,seq=42i,margin_snr=23.75 1762400000000000000
"""

import json
from typing import Iterable, Iterator, Optional

from .db import Store


def _escape_tag(v: str) -> str:
    return str(v).replace("\\", "\\\\").replace(",", "\\,").replace("=", "\\=").replace(" ", "\\ ")


def _field(v) -> Optional[str]:
    if v is None:
        return None
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return "%di" % v
    if isinstance(v, float):
        return repr(v)
    return '"%s"' % str(v).replace("\\", "\\\\").replace('"', '\\"')


def line(measurement: str, tags: dict, fields: dict, t_seconds: float) -> Optional[str]:
    tag_s = "".join(",%s=%s" % (_escape_tag(k), _escape_tag(v)) for k, v in sorted(tags.items()) if v not in (None, ""))
    field_s = ",".join("%s=%s" % (_escape_tag(k), f) for k, f in ((k, _field(v)) for k, v in sorted(fields.items()))
                       if f is not None)
    if not field_s:
        return None
    return "%s%s %s %d" % (_escape_tag(measurement), tag_s, field_s, int(round(t_seconds * 1e9)))


_TETHER_FIELDS = ("rssi", "snr", "seq", "margin_snr", "margin_rssi", "interval_ms", "airtime_ms", "sent", "replies",
                  "pdr32", "heard", "replied", "missed", "loss", "skipped", "passive", "backoff", "rssi_ewma",
                  "snr_ewma", "rpt_rssi_ewma", "rpt_snr_ewma", "ch_util", "air_util_tx")


def export(store: Store, since: float = 0.0) -> Iterator[str]:
    for r in store.query("SELECT t, node_id, fields FROM tether WHERE t > ? ORDER BY t", (since,)):
        f = json.loads(r["fields"])
        ev = f.get("event")
        tags = {"node": r["node_id"], "peer": f.get("peer"), "kind": f.get("kind"), "reason": f.get("reason"),
                "role": f.get("role")}
        fields = {k: f[k] for k in _TETHER_FIELDS if k in f}
        out = line("tether_" + str(ev), tags, fields, r["t"])
        if out:
            yield out
    for r in store.query("SELECT t, from_id, portnum, rssi, snr, hop_start, hop_limit FROM packets "
                         "WHERE rssi IS NOT NULL AND via_mqtt=0 AND t > ? ORDER BY t", (since,)):
        tags = {"from": r["from_id"], "port": r["portnum"], "direct": "1" if r["hop_start"] == r["hop_limit"] else "0"}
        out = line("mesh_rx", tags, {"rssi": r["rssi"], "snr": r["snr"]}, r["t"])
        if out:
            yield out


def write_file(lines: Iterable[str], path: str) -> int:
    n = 0
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for ln in lines:
            fh.write(ln + "\n")
            n += 1
    return n
