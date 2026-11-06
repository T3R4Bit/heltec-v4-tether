# First hardware test (two Heltec V4 boards)

Nothing in TETHER has run on a board yet. This is the checklist that turns the `[~]` items in PLANNED_FEATURES.md
into `[x]` (or into bug reports). Allow about an hour, plus a walk for the range part.

> **Antennas on both boards before powering them.** Transmitting without an antenna can damage the radio front end.

## 0. Build and flash (each board)
```sh
python scripts/build_firmware.py --upload --port COM5          # Windows, from PowerShell
python scripts/build_firmware.py --upload --port /dev/ttyUSB0  # Linux; macOS /dev/cu.usbserial-* or /dev/cu.usbmodem*
```
Add `--layout 2` or `--layout 3` to try the other OLED layouts ([OLED.md](OLED.md)).
Alternative: flash `firmware-heltec-v4-2.8.2.d5f1619.factory.bin` at offset 0 with the Meshtastic web flasher or
`esptool --chip esp32s3 write_flash 0x0 <file>` (full erase: settings reset).
Meshtastic 2.8.2 is a **pre-release**; if the boards held settings from older firmware, re-check region and channel.

## 1. Configure (phone app or `meshtastic` CLI)
- Same **region**, **channel** and **preset** on both (LongFast is fine).
- On each board, **favorite the other board, and nothing else**.
- `device.rebroadcast_mode` not `CORE_PORTNUMS_ONLY`.
- On the board that will be plugged into the laptop: `meshtastic --set security.debug_log_api_enabled true`.
- Note both node numbers. The **lower** one is the initiator (sends pings).

## 2. Watch the serial log
`pio device monitor -b 115200 --port COM5`, or the logger (`python tools/logger.py --port COM5 --db bench.db`), which also
prints every `TETHER` line. Expected, in order:

| When | Initiator (lower node number) | Responder |
|---|---|---|
| Boot | `TETHER boot version=1 ...` | same |
| ~30-40 s | `TETHER peer peer=!xxxxxxxx role=initiator source=favorite` | `... role=responder ...` |
| Every ~12 s (LongFast) | `TETHER tx kind=ping ... interval_ms≈12000` then `TETHER rx kind=reply` and `TETHER rpt kind=ping` | `TETHER rx kind=ping`, `TETHER tx kind=reply`, and from the 2nd ping on `TETHER rpt kind=reply` |
| Every 60 s | `TETHER stats ... role=initiator ... pdr32=...` and `TETHER power ...` | `TETHER stats ... role=responder ...` and `TETHER power ...` |

## 3. Tests: record the result of each (pass/fail + a log excerpt)
| # | Test | Expected |
|---|---|---|
| 1 | Both boards on a desk for 5 min | `pdr32` ≥ 0.95 on the initiator; responder `loss` ≈ 0 |
| 2 | Ping interval | Matches the preset: LongFast 10.8-13.2 s between `tx kind=ping` (±10 % jitter) |
| 3 | Back-off | Unplug the responder: after 3 unanswered pings the gap doubles, then x4, x8 (`backoff=` field). Plug back in: back to x1 after the first reply |
| 4 | Passive samples | Send a text from the responder's app: initiator logs `TETHER rx kind=passive ... port=1` |
| 5 | No peer | Un-favorite on one board: `TETHER idle reason=no_peer` within 10 s, OLED shows NO PEER |
| 6 | Two favorites | Favorite a second node: same as 5 (several favorites = no peer) |
| 7 | OLED | TETHER page appears in the screen carousel; bars move; LOST badge ~90 s after unplugging the peer |
| 8 | GPS (V4 GNSS header fitted) | Outdoors: `rx` lines end in `lat=... gps=ok`; indoors / poor sky: `gps=nofix`, `gps=sats` or `gps=hdop` |
| 9 | Battery | On battery: `power` shows `pct`; after 15 min `trend_pct_h` and `runtime_h` appear; on USB `charging=1` |
| 10 | Channel gate | Rarely triggers on a quiet channel; if `TETHER skip reason=chutil` ever appears, confirm no ping was sent in that slot |
| 11 | Laptop tools | Logger + dashboard on the initiator: tiles fill, map shows the peer once it has a GPS fix |

## 4. Range walk
Base board plugged into the laptop (logger + dashboard), peer carried with GPS on. Walk out until the dashboard shows
LOST, then back. Keep the `.db` file. That is the first real dataset, and the only kind of data that belongs on a
website as results (screenshots in `docs/screenshots/` today are synthetic and say so).

## 5. Send back
Both serial logs, the `.db` file, OLED photos of the layout(s) you tried, and the results table above. Then update
PLANNED_FEATURES.md: `[x]` only for what passed on the boards.
