# Planned Features

Legend: `[ ]` not started · `[~]` written, untested · `[x]` built **and** tested on hardware

## Phase 0: Toolchain
See [docs/BUILD.md](docs/BUILD.md). Compile-only items have no hardware part; `[x]` there means the build was run and succeeded.
- [x] Clone `meshtastic/firmware`, pinned to `v2.8.2.d5f1619` (first hook-capable line is 2.8.1; all 2.8.x are pre-releases). `./firmware` is gitignored; a path without spaces is required, so `TETHER_FIRMWARE_DIR` can point elsewhere
- [x] Unmodified `heltec-v4` env builds with PlatformIO (2026-11-05, Windows; not flashed)
- [~] `scripts/install_module.py`: copies `module/Tether/` into `src/modules/optional/`. Tested against throwaway directory trees only; not yet exercised with a real module in a real build

## Phase 1: Core link module (MVP)
Fixed pair of known nodes (each board is configured with the peer's node ID). Packet format and data flow go in `docs/DESIGN_NOTES.md` before code.
- [ ] Module skeleton: `SinglePortModule` + `OSThread` on `PRIVATE_APP` (port 256), registered via `OPTIONAL_MODULES_SETUP`
- [ ] Active ping/reply, hop_limit 0, gated by `airTime->isTxAllowedChannelUtil()`
- [ ] Passive RSSI/SNR logging from normal traffic
- [ ] Two-way stats (my RX of them, their RX of me)
- [ ] Ring-buffer history
- [ ] Fade margin vs. the current preset's sensitivity
- [ ] Ping interval scaled to preset airtime (LongFast cannot do 1 Hz; expect 5-15 s on long presets, faster on short)
- [ ] Serial output of stats

## Phase 2: Feedback
- [ ] Scrolling RSSI/SNR graph on OLED (UI style TBD, see Open Decisions)
- [ ] Proximity tone: pitch/rate follows signal (blocked: buzzer GPIO not chosen yet; compiled out until then)
- [ ] GPS-tagged signal log for fox-hunting (V4 GNSS header)
  - [ ] Log HDOP, satellite count and fix type per sample
  - [ ] Drop samples with HDOP above ~2 or fewer than ~6 satellites
  - [ ] Smooth position
  - [ ] Higher local fix rate while fox-hunting
  - [ ] Timestamp samples from the GNSS fix

## Phase 3: Power
- [ ] Smoothed VBAT
- [ ] TX voltage-sag log
- [ ] Charge/solar detection
- [ ] Runtime estimate
- [ ] Adaptive low-battery behaviour: lower ping rate, brightness, TX power; last-gasp packet
- [ ] Optional INA219/226/260 coulomb counting (mA/mAh)

## Phase 4: Export and sweeps
- [ ] Serial/MQTT lines in InfluxDB line protocol for Grafana (self-hosted)
- [ ] Automated preset / TX-power sweep for range tests

## Phase 4b: Laptop-side logger and dashboard (companion tools)
Runs on a laptop connected to a node (serial/USB, or TCP). Separate from the firmware module; lives in `tools/`. Python, local SQLite, self-hosted only.
- [ ] `logger.py`: record everything the connected node hears (messages, GPS/position updates, RSSI/SNR) into SQLite with timestamps
- [ ] `logger.py`: send messages from the terminal
- [ ] `dashboard.py`: live browser dashboard reading the same database
  - [ ] Map of the remote node's position
  - [ ] SNR (and RSSI) over time
  - [ ] Signal vs. distance chart (the key range-test graph)
  - [ ] Max distance reached
  - [ ] Message history with signal info per message
- [ ] Optionally ingest TETHER's port-256 link stats once Phase 1 exists, alongside normal traffic
- [ ] Testable without hardware: replay a sample dataset, clearly labelled as synthetic
- [ ] Screenshots / sample dataset from a real range test (only once actually run)

## Phase 4c: Mobile web app (PWA)
No native iOS app. The dashboard becomes a mobile-first PWA, self-hosted.
- [ ] Mobile-first layout
- [ ] Web app manifest and icons; add to iOS Home Screen
- [ ] Served over LAN / Tailscale (no paid cloud)
- [ ] Optional phone geolocation as a position source, matched to RSSI samples by timestamp

## Phase 5: Verify
- [ ] Compile with PlatformIO (`heltec-v4`)
- [ ] Hardware test on two V4 boards
- [ ] Run logger/dashboard against a real range test
- [ ] Record plainly what remains untested

## Constraints
- OLED UI is hard-edged and blocky (neo-brutalist); no invented styling. Ask before choosing a look.
- Never add traffic that ignores channel utilization.
- Self-hosted, no paid cloud services.
- Absolute RSSI is not comparable across V4.2 / V4.3 / V3; use relative changes on the same hardware.

## Decisions
- [x] Pairing: **fixed pair of known nodes** (any-node out of scope for now)
- [x] Target env: **OLED, `heltec-v4`** (TFT not targeted)
- [x] Phone interface: **mobile-first web app / PWA** (Phase 4c); no native app, stock Meshtastic app will not show TETHER data
- [x] Name: **TETHER**, repo **`heltec-v4-tether`**

## Open Decisions (to resolve, not assume)
- [ ] Buzzer: passive buzzer on which free GPIO? Deferred; no buzzer for now.
- [ ] OLED UI look: propose 2-3 neo-brutalist sketches and ask before building (Phase 2)
