"""SQLite storage for the logger and dashboard. One file per range test is a good habit."""

import json
import sqlite3
import threading
import time
from typing import Optional

SCHEMA_VERSION = 1

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY,
    started REAL NOT NULL,
    source TEXT NOT NULL,          -- 'live', 'synthetic', 'replay'
    synthetic INTEGER NOT NULL DEFAULT 0,
    local_node TEXT,
    note TEXT
);
CREATE TABLE IF NOT EXISTS packets (
    id INTEGER PRIMARY KEY,
    session INTEGER NOT NULL,
    t REAL NOT NULL,
    packet_id INTEGER,
    from_id TEXT,
    to_id TEXT,
    portnum TEXT,
    channel INTEGER,
    rssi INTEGER,
    snr REAL,
    hop_start INTEGER,
    hop_limit INTEGER,
    via_mqtt INTEGER NOT NULL DEFAULT 0,
    text TEXT
);
CREATE INDEX IF NOT EXISTS packets_from_t ON packets(from_id, t);
CREATE TABLE IF NOT EXISTS positions (
    id INTEGER PRIMARY KEY,
    session INTEGER NOT NULL,
    t REAL NOT NULL,
    node_id TEXT NOT NULL,
    lat REAL NOT NULL,
    lon REAL NOT NULL,
    alt REAL,
    sats INTEGER,
    dop REAL,
    accuracy_m REAL,
    source TEXT NOT NULL           -- 'mesh' (position packet) or 'phone' (browser geolocation)
);
CREATE INDEX IF NOT EXISTS positions_node_t ON positions(node_id, t);
CREATE TABLE IF NOT EXISTS tether (
    id INTEGER PRIMARY KEY,
    session INTEGER NOT NULL,
    t REAL NOT NULL,
    node_id TEXT,                  -- node that printed the line (the one attached to the logger)
    event TEXT NOT NULL,           -- rx, rpt, tx, stats, skip, idle, gap, ...
    kind TEXT,
    peer TEXT,
    seq INTEGER,
    rssi INTEGER,
    snr REAL,
    margin_snr REAL,
    margin_rssi REAL,
    fields TEXT NOT NULL           -- every parsed key=value as JSON
);
CREATE INDEX IF NOT EXISTS tether_event_t ON tether(event, t);
CREATE TABLE IF NOT EXISTS sent (
    id INTEGER PRIMARY KEY,
    session INTEGER NOT NULL,
    t REAL NOT NULL,
    to_id TEXT,
    text TEXT
);
"""


class Store:
    """Thin wrapper around a SQLite connection. Safe to share between the logger's threads."""

    def __init__(self, path: str):
        self.path = path
        self._lock = threading.Lock()
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")  # dashboard can read while the logger writes
        self.conn.executescript(SCHEMA)
        self.conn.execute("INSERT OR IGNORE INTO meta(key, value) VALUES ('schema_version', ?)", (str(SCHEMA_VERSION),))
        self.conn.commit()

    def close(self) -> None:
        with self._lock:
            self.conn.close()

    def _insert(self, sql: str, args) -> int:
        with self._lock:
            cur = self.conn.execute(sql, args)
            self.conn.commit()
            return cur.lastrowid

    def new_session(self, source: str, synthetic: bool = False, local_node: Optional[str] = None,
                    note: Optional[str] = None, started: Optional[float] = None) -> int:
        return self._insert(
            "INSERT INTO sessions(started, source, synthetic, local_node, note) VALUES (?,?,?,?,?)",
            (started if started is not None else time.time(), source, 1 if synthetic else 0, local_node, note),
        )

    def set_local_node(self, session: int, local_node: str) -> None:
        with self._lock:
            self.conn.execute("UPDATE sessions SET local_node=? WHERE id=?", (local_node, session))
            self.conn.commit()

    def add_packet(self, session: int, t: float, **kw) -> int:
        cols = ["session", "t"] + list(kw.keys())
        return self._insert(
            "INSERT INTO packets(%s) VALUES (%s)" % (",".join(cols), ",".join("?" * len(cols))),
            [session, t] + list(kw.values()),
        )

    def add_position(self, session: int, t: float, node_id: str, lat: float, lon: float, source: str = "mesh",
                     alt=None, sats=None, dop=None, accuracy_m=None) -> int:
        return self._insert(
            "INSERT INTO positions(session,t,node_id,lat,lon,alt,sats,dop,accuracy_m,source) VALUES (?,?,?,?,?,?,?,?,?,?)",
            (session, t, node_id, lat, lon, alt, sats, dop, accuracy_m, source),
        )

    def add_tether(self, session: int, t: float, node_id: Optional[str], parsed: dict) -> int:
        return self._insert(
            "INSERT INTO tether(session,t,node_id,event,kind,peer,seq,rssi,snr,margin_snr,margin_rssi,fields) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (session, t, node_id, parsed.get("event"), parsed.get("kind"), parsed.get("peer"), parsed.get("seq"),
             parsed.get("rssi"), parsed.get("snr"), parsed.get("margin_snr"), parsed.get("margin_rssi"),
             json.dumps(parsed, sort_keys=True)),
        )

    def add_sent(self, session: int, t: float, to_id: Optional[str], text: str) -> int:
        return self._insert("INSERT INTO sent(session,t,to_id,text) VALUES (?,?,?,?)", (session, t, to_id, text))

    def query(self, sql: str, args=()):
        with self._lock:
            return [dict(r) for r in self.conn.execute(sql, args).fetchall()]
