# Planned Features

Legend: `[ ]` not started · `[~]` written, untested · `[x]` built **and** tested on hardware

## Phase 1: Core link module (MVP)
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
- [ ] Proximity tone: pitch/rate follows signal
- [ ] GPS-tagged signal log for fox-hunting (V4 GNSS header)

## Phase 3: Power
- [ ] Smoothed VBAT
- [ ] TX voltage-sag log
- [ ] Charge/solar detection
- [ ] Runtime estimate
- [ ] Adaptive low-battery behaviour: lower ping rate, brightness, TX power; last-gasp packet
- [ ] Optional INA219/226/260 coulomb counting (mA/mAh)

## Phase 4: Export and sweeps
- [ ] Serial/MQTT lines formatted for InfluxDB/Grafana (self-hosted)
- [ ] Automated preset / TX-power sweep for range tests

## Phase 5: Verify
- [ ] Compile with PlatformIO (`heltec-v4`, and `heltec-v4-tft` if chosen)
- [ ] Hardware test on two V4 boards
- [ ] Record plainly what remains untested

## Constraints
- OLED UI is hard-edged and blocky (neo-brutalist); no invented styling. Ask before choosing a look.
- Never add traffic that ignores channel utilization.
- Self-hosted, no paid cloud services.
- Absolute RSSI is not comparable across V4.2 / V4.3 / V3; use relative changes on the same hardware.

## Open Decisions (to resolve, not assume)
- [ ] Fixed pair of known nodes, or any node in range?
- [ ] OLED env or TFT env?
- [ ] Phone-app display (large extra job) or OLED/serial/MQTT only?
- [ ] Buzzer: passive buzzer on which free GPIO?
- [ ] Final project name and repo name (currently `meshtastic-tether`; repo is `heltec-v4-tether`)
