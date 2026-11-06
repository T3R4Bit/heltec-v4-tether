"""Laptop-side tools for TETHER: logging a Meshtastic node to SQLite and serving a live dashboard.

Standard library only, except the live logger, which needs the `meshtastic` package to talk to a node.
"""

__all__ = ["db", "parse", "ingest", "analysis", "lineproto"]
