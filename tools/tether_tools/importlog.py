"""Import a saved serial log (e.g. from `pio device monitor`) into the database.

Only TETHER lines are stored. Timestamps come from one consistent time base:
  - the HH:MM:SS clock Meshtastic prints at the start of each log line, read as UTC on `date`, rolling over at
    midnight; if any GPS-tagged lines carry both that clock and gps_t (GNSS epoch), their median difference calibrates
    the clock (e.g. a device clock set to local time, or slightly off)
  - lines without a clock use gps_t if they have it, else one second after the previous line
"""

import datetime as _dt
import re
import statistics
from pathlib import Path
from typing import Iterable, List, Optional

from .db import Store
from .ingest import Ingestor
from .parse import parse_tether_line

_CLOCK_RE = re.compile(r"\b(\d{2}):(\d{2}):(\d{2})\b")


def _utc_midnight(date: _dt.date) -> float:
    return _dt.datetime(date.year, date.month, date.day, tzinfo=_dt.timezone.utc).timestamp()


def import_lines(store: Store, lines: Iterable[str], date: _dt.date, note: str = "import",
                 local_node: Optional[str] = None) -> Ingestor:
    day0 = _utc_midnight(date)

    # Pass 1: TETHER lines with their clock (seconds since `date` 00:00 UTC, rollover applied) and GNSS time.
    entries: List[tuple] = []
    last_clock, day_offset = None, 0
    for raw in lines:
        line = raw.rstrip("\r\n")
        parsed = parse_tether_line(line)
        if not parsed:
            continue
        m = _CLOCK_RE.search(line[: line.find("TETHER")])  # the clock is in the prefix, before the TETHER part
        clock = None
        if m:
            c = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            if last_clock is not None and c < last_clock - 3600:
                day_offset += 86400  # log ran past midnight
            last_clock = c
            clock = c + day_offset
        gps_t = parsed.get("gps_t")
        gps_t = float(gps_t) if isinstance(gps_t, int) and gps_t > 1_000_000_000 else None
        entries.append((line, clock, gps_t))

    diffs = [g - (day0 + c) for _, c, g in entries if c is not None and g is not None]
    offset = statistics.median(diffs) if diffs else 0.0

    # Pass 2: store with consistent timestamps.
    session = store.new_session("import", note=note, local_node=local_node)
    ing = Ingestor(store, session, local_node)
    t = day0
    for line, clock, gps_t in entries:
        if clock is not None:
            t = day0 + clock + offset
        elif gps_t is not None:
            t = gps_t
        else:
            t = t + 1.0
        ing.ingest_log_line(line, t)
    return ing


def import_file(store: Store, path: str, date: Optional[_dt.date] = None, local_node: Optional[str] = None) -> Ingestor:
    p = Path(path)
    if date is None:
        date = _dt.datetime.fromtimestamp(p.stat().st_mtime, tz=_dt.timezone.utc).date()
    with open(p, encoding="utf-8", errors="replace") as fh:
        return import_lines(store, fh, date, note="serial log %s" % p.name, local_node=local_node)
