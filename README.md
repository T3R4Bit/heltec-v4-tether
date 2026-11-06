# TETHER

A Meshtastic firmware add-on module for the **Heltec WiFi LoRa 32 V4** (ESP32-S3R2, SX1262 + front-end module), plus a laptop logger and a mobile web app.

> **Status: Phase 0 (toolchain) done.** A stock `heltec-v4` build compiles; no TETHER firmware has been written, built, or tested yet.
> Do not assume any feature works until it is marked tested in [PLANNED_FEATURES.md](PLANNED_FEATURES.md).

## What it adds
1. **Live link quality** between a fixed pair of nodes: continuous two-way RSSI/SNR, history, and fade margin, shown on the device and exportable.
2. **Better power tracking**: smoothed battery readings, TX voltage sag, charge/solar detection, runtime estimate, optional true mA/mAh via an external INA219/226/260 on I2C.
3. **Laptop logger and dashboard** (planned, `tools/`): `logger.py` saves everything a connected node hears (messages, GPS, signal) to a local SQLite database with timestamps and can send messages from the terminal. `dashboard.py` shows it live in a browser: node map, SNR over time, signal vs. distance, max distance reached, and message history with signal info. This turns a range test into real graphs and numbers instead of guesses.
4. **Mobile web app** (planned): the dashboard as a mobile-first PWA you can add to an iPhone Home Screen, self-hosted over LAN or Tailscale. No native iOS app.

## Hardware notes
- Target env: `heltec-v4` (OLED, SSD1306-compatible). `heltec-v4-tft` exists upstream but is not targeted.
- Battery ADC: `BATTERY_PIN` 1, enable via `ADC_CTRL` 37. **No current sensor on the board**; real current needs an external INA part.
- OLED env I2C: SDA 17 / SCL 18, second bus 4 / 3. Only the TFT env defines a buzzer (GPIO 6); the OLED env does not. Buzzer pin is deferred, so the proximity tone stays compiled out for now.
- V4.2 uses a GC1109 front end, V4.3 a KCT8103L. RX behaviour differs, so absolute RSSI is not comparable across revisions or against V3. Use relative changes on the same hardware.

## How it hooks into Meshtastic
- Lives in its own folder under `src/modules/optional/` via the `OPTIONAL_MODULES_SETUP` hook. No core files patched, so upstream updates stay easy.
- Modelled on `RangeTestModule` (`SinglePortModule` + `OSThread`, hop_limit 0, checks `airTime->isTxAllowedChannelUtil()` before sending).
- Uses `PRIVATE_APP` (port 256). The stock phone app will **not** display this data; output goes to OLED, serial and MQTT.
- Reuses existing Meshtastic pieces: per-packet `rx_rssi`/`rx_snr`, the LiPo OCV curve in `Power.h`, and the INA219/226/260 telemetry sensors.

## Repo layout
```
module/Tether/   module source, copied into a firmware checkout (empty for now)
tools/           laptop-side logger.py and dashboard.py (planned, not created yet)
docs/            design notes, open decisions
scripts/         install_module.py (copies the module into a firmware checkout)
PLANNED_FEATURES.md
```
This is a standalone repo that copies the module into a Meshtastic firmware checkout. It is **not** a fork.

## Getting started
Full steps and Windows gotchas: [docs/BUILD.md](docs/BUILD.md).
1. Clone Meshtastic firmware **v2.8.2.d5f1619** (needs ≥ v2.8.1 for the optional-module hook) into `./firmware` (gitignored), or anywhere without spaces in the path and set `TETHER_FIRMWARE_DIR`.
2. `python scripts/install_module.py` copies `module/Tether/` into the checkout's `src/modules/optional/`.
3. `cd <firmware> && pio run -e heltec-v4`

## Principles
- MVP (phase 1) working end to end before polishing later phases. Architecture and data flow first, UI last.
- Self-hosted only, no paid cloud services.
- Respect airtime and duty-cycle limits; never send traffic that ignores channel utilization.
- Nothing is claimed to work unless it was built and tested.

## License
GPL-3.0 (links into Meshtastic). See [LICENSE](LICENSE).
