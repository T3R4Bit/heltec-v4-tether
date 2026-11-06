# TETHER (working name)

A Meshtastic firmware add-on module for the **Heltec WiFi LoRa 32 V4** (ESP32-S3R2, SX1262 + front-end module).

> **Status: planning only.** Nothing here has been written as firmware, built, or tested yet.
> Do not assume any feature works until it is marked tested in [PLANNED_FEATURES.md](PLANNED_FEATURES.md).

## What it adds
1. **Live link quality** between two nodes: continuous two-way RSSI/SNR, history, and fade margin, shown on the device and exportable.
2. **Better power tracking**: smoothed battery readings, TX voltage sag, charge/solar detection, runtime estimate, optional true mA/mAh via an external INA219/226/260 on I2C.

## Hardware notes
- PlatformIO envs: `heltec-v4` (OLED, SSD1306-compatible) and `heltec-v4-tft`.
- Battery ADC: `BATTERY_PIN` 1, enable via `ADC_CTRL` 37. **No current sensor on the board**; real current needs an external INA part.
- OLED env I2C: SDA 17 / SCL 18, second bus 4 / 3. Only the TFT env defines a buzzer (GPIO 6); the OLED env does not, so the buzzer GPIO must be chosen.
- V4.2 uses a GC1109 front end, V4.3 a KCT8103L. RX behaviour differs, so absolute RSSI is not comparable across revisions or against V3. Use relative changes on the same hardware.

## How it hooks into Meshtastic
- Lives in its own folder under `src/modules/optional/` via the `OPTIONAL_MODULES_SETUP` hook. No core files patched, so upstream updates stay easy.
- Modelled on `RangeTestModule` (`SinglePortModule` + `OSThread`, hop_limit 0, checks `airTime->isTxAllowedChannelUtil()` before sending).
- Uses `PRIVATE_APP` (port 256). The stock phone app will **not** display this data; output goes to OLED, serial and MQTT.
- Reuses existing Meshtastic pieces: per-packet `rx_rssi`/`rx_snr`, the LiPo OCV curve in `Power.h`, and the INA219/226/260 telemetry sensors.

## Repo layout
```
module/Tether/   module source, copied into a firmware checkout (empty for now)
docs/            design notes, open decisions
scripts/         helper scripts (e.g. install module into a firmware checkout)
PLANNED_FEATURES.md
```
This is a standalone repo that copies the module into a Meshtastic firmware checkout. It is **not** a fork.

## Getting started (planned)
1. Clone Meshtastic firmware next to this repo.
2. Run the (not yet written) install script to copy `module/Tether/` into `firmware/src/modules/optional/`.
3. `pio run -e heltec-v4`

## Principles
- MVP (phase 1) working end to end before polishing later phases. Architecture and data flow first, UI last.
- Self-hosted only, no paid cloud services.
- Respect airtime and duty-cycle limits; never send traffic that ignores channel utilization.
- Nothing is claimed to work unless it was built and tested.

## License
GPL-3.0 (links into Meshtastic). See [LICENSE](LICENSE).
