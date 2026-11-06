# Planned Features

Legend: `[ ]` not started · `[~]` written, untested · `[x]` built **and** tested on hardware

## Phase 0: Toolchain
See [docs/BUILD.md](docs/BUILD.md). Compile-only items have no hardware part; `[x]` there means the build was run and succeeded.
- [x] Clone `meshtastic/firmware`, pinned to `v2.8.2.d5f1619` (first hook-capable line is 2.8.1; all 2.8.x are pre-releases). `./firmware` is gitignored; a path without spaces is required, so `TETHER_FIRMWARE_DIR` can point elsewhere
- [x] Unmodified `heltec-v4` env builds with PlatformIO (2026-11-05, Windows; not flashed)
- [x] `scripts/install_module.py`: copies `module/Tether/` into `src/modules/optional/`. Used for the real Phase 1 build (the hook printed `optional-modules: Tether`)

## Phase 1: Core link module (MVP)
Fixed pair of known nodes: the peer is the single favorited node (or `-DTETHER_PEER_NODENUM`). Design: [docs/DESIGN_NOTES.md](docs/DESIGN_NOTES.md).

**Status:** all items written, the `heltec-v4` firmware with the module **compiles**, and the hardware-independent
logic (wire format, link math, stats) passes **host unit tests** (`python scripts/run_host_tests.py`). **Nothing has
run on a board yet**, so every item stays `[~]` until the two-board hardware test (design notes, section 9 step 3).
- [~] Module skeleton: `SinglePortModule` + `OSThread` on `PRIVATE_APP` (port 256), registered via `OPTIONAL_MODULES_SETUP` (registration confirmed at build time only)
- [~] Active ping/reply, hop_limit 0, gated by `isTxAllowedChannelUtil(true)` **and** `isTxAllowedAirUtil()`; lower node number initiates; back-off after 3 missed replies
- [~] Passive RSSI/SNR logging from normal traffic (direct packets only; best-effort, see design notes 5)
- [~] Two-way stats (my RX of them, their RX of me)
- [~] Ring-buffer history (128 samples per direction)
- [~] Fade margin vs. the current preset's sensitivity (SNR-based and RSSI-based)
- [~] Ping interval scaled to preset airtime (10 % budget: LongFast 12 s, ShortFast 2 s, LongSlow 71 s)
- [~] Serial output of stats (`TETHER ...` key=value log lines)
- [x] Host unit tests for protocol, math and stats (160 checks; no hardware involved)

## Phase 2: Feedback
Firmware compiles with all of this (2026-11-06); pure logic host-tested (73 checks); **nothing seen on a real board yet.**
- [~] Scrolling SNR graph on OLED: three candidate layouts, real renders in [docs/OLED.md](docs/OLED.md). **Look not
  chosen yet**: layout 1 is a provisional default (`-DTETHER_OLED_LAYOUT=1|2|3`)
- [ ] Proximity tone: pitch/rate follows signal (blocked: buzzer GPIO not chosen yet)
- [~] GPS-tagged signal log for fox-hunting: every `TETHER rx`/`rpt` line carries the local fix (`TETHER_GPS_TAG`)
  - [~] Log HDOP, satellite count and fix type per sample
  - [~] Drop samples with HDOP above 2 or fewer than 6 satellites (logged as `gps=hdop|sats|nofix` instead)
  - [~] Smooth position (EWMA per new fix, restarts on a >100 m jump)
  - [ ] Higher local fix rate while fox-hunting (not done: would mean rewriting the user's GPS update interval at runtime)
  - [~] Timestamp samples from the GNSS fix (`gps_t`)

## Phase 3: Power
Firmware compiles; pure logic host-tested; not run on a board. Uses Meshtastic's PowerStatus (its LiPo OCV curve), no ADC code of its own.
- [~] Smoothed VBAT and charge (`TETHER power` line every 60 s)
- [ ] TX voltage-sag log (not done: needs ADC reads timed around TX, and the battery ADC belongs to Meshtastic's Power class)
- [~] Charge detection (USB / charging flags from PowerStatus). Solar is not distinguished from USB charging
- [~] Runtime estimate (least-squares slope of charge over the last hour, needs 15 min of history)
- [~] Adaptive low battery: ping interval x2 below 20 %, x4 below 10 % when not charging
  - [ ] Lower brightness / TX power, last-gasp packet (not done: these change the user's device config or the wire format; want a decision first)
- [ ] Optional INA219/226/260 coulomb counting (mA/mAh)

## Phase 4: Export and sweeps
- [~] InfluxDB line protocol for Grafana (self-hosted): done **on the laptop side** (`logger.py --export-lp`), unit-tested
  on synthetic data. Not emitted by the firmware itself, and no MQTT publishing from the firmware yet
- [ ] Automated preset / TX-power sweep for range tests (not started: needs both nodes to switch radio settings in step,
  which risks stranding them on different presets; wants hardware to develop against)

## Phase 4b: Laptop-side logger and dashboard (companion tools)
Lives in [`tools/`](tools/README.md). Python, local SQLite, self-hosted. **Tested without hardware only**: 25 unit tests,
synthetic replay, and a real-browser end-to-end test (headless Edge via Playwright, phone and desktop sizes, no JS
errors). Nothing has been run against a real node yet, so items stay `[~]`.
- [~] `logger.py`: record everything the connected node hears (messages, positions, RSSI/SNR, TETHER lines) into SQLite with timestamps (live path checked against the installed `meshtastic` 2.7.11 API, not run against a node)
- [~] `logger.py`: send messages from the terminal (broadcast, `/dm !id text`)
- [~] `dashboard.py`: live browser dashboard reading the same database
  - [~] Map of the remote node's position (Leaflet vendored; OSM tiles need internet)
  - [~] SNR and RSSI over time, both directions
  - [~] Signal vs. distance chart
  - [~] Max distance reached (only counts packets actually received)
  - [~] Message history with signal info per message
- [~] Ingest TETHER's link stats (`TETHER` log lines via `security.debug_log_api_enabled`)
- [~] Without the API: `--raw-serial PORT` and `--import-log FILE` store TETHER lines (GPS tags become positions);
  `--peer-pos` for a stationary peer. Unit-tested with sample log text; not run against a real serial port
- [x] Testable without hardware: `tools/synth.py` generates a link-model dataset, replay marks it synthetic, dashboard shows a banner
- [ ] Screenshots / sample dataset from a real range test (only once actually run; current screenshots are synthetic and say so)

## Phase 4c: Mobile web app (PWA)
- [~] Mobile-first layout (checked at 390 px wide in a real browser: no horizontal scroll)
- [~] Web app manifest, icons, service worker; add to iOS Home Screen (files served and validated in tests; not tried on an iPhone)
- [~] Served over LAN / Tailscale (`--host 0.0.0.0`, `tailscale serve`, optional `--cert/--key`); not tried over Tailscale
- [~] Optional phone geolocation as a position source, matched to RSSI samples by timestamp (`POST /api/geo`, tested via HTTP; not tried on a phone)

## Phase 5: Verify
- [x] Compile with PlatformIO (`heltec-v4`): `python scripts/build_firmware.py` builds with no TETHER warnings (compile only, 2026-11-06)
- [ ] Hardware test on two V4 boards: checklist in [docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md)
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
- [ ] OLED UI look: three layouts proposed with real renders in [docs/OLED.md](docs/OLED.md). Pick 1, 2 or 3 (or ask for changes).
