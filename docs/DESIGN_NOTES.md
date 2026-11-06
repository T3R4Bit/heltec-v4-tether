# Design Notes (Phase 1: core link module)

Written before code. Every firmware claim below was checked against Meshtastic **v2.8.2.d5f1619** source;
file references are relative to the firmware checkout.

## 1. Pieces and where they hook in

```
                 Meshtastic core (untouched)
 ┌──────────────────────────────────────────────────────────────────┐
 │ RadioLib RX → Router::dispatchReceived → perhapsDecode (PKI/PSK)  │
 │        → MeshModule::callModules(mp)   (modules in setup order)   │
 └───────────────┬───────────────────────────────┬──────────────────┘
                 │ port 256, to == us            │ any packet, promiscuous
                 ▼                               ▼
     ┌───────────────────────┐        ┌──────────────────────────┐
     │ TetherModule          │        │ TetherSniffer            │
     │ SinglePortModule(256) │        │ MeshModule, promiscuous, │
     │ + OSThread            │        │ encryptedOk              │
     │  - ping schedule      │        │  - from == peer, direct, │
     │  - reply to pings     │        │    not port 256          │
     └──────────┬────────────┘        └────────────┬─────────────┘
                │      samples (rssi, snr, seq)     │
                ▼                                   ▼
          ┌──────────────────────────────────────────────┐
          │ TetherStats: two ring buffers + aggregates   │
          │  local  = my RX of peer                       │
          │  remote = peer's RX of me (reported in-band)  │
          └───────────────┬──────────────────────────────┘
                          ▼
                  Serial (LOG_INFO "TETHER ..." lines)
                  [OLED Phase 2, MQTT/line protocol Phase 4]
```

Registration: `module/Tether/Tether.h` declares `void setupTether();`. `bin/optional-modules.py` includes it
and `Modules.cpp` calls it via `OPTIONAL_MODULES_SETUP()` **after** every stock module and **before**
`RoutingModule`. No core file is patched.

Source split, so the logic can be unit-tested on a PC without Arduino headers:

| File | Contents | Depends on Meshtastic? |
|---|---|---|
| `TetherProtocol.h` | Wire format, encode/decode | No |
| `TetherMath.h` | SNR floors, sensitivity, fade margin, LoRa airtime, ping interval | No |
| `TetherStats.h` | Ring buffers, aggregates, loss counters | No |
| `TetherModule.{h,cpp}` | Peer selection, scheduling, send/receive, sniffer, logging | Yes |
| `Tether.h` / `Tether.cpp` | `setupTether()` | Yes |

## 2. Peer (fixed pair)

Resolved on every scheduler tick, first match wins:
1. Build flag `-DTETHER_PEER_NODENUM=0x........` (non-zero).
2. Otherwise, **exactly one favorited node** in NodeDB (`NodeDB::isFavorite`). Favoriting is done from the stock
   phone app (star the node), so no custom tool is needed.
3. Otherwise no peer: the module sends nothing and logs `TETHER idle reason=no_peer` at most once a minute.

Zero or several favorites, or the peer equal to our own node number, count as "no peer". Packets from any other node are ignored.

**Roles.** Both boards point at each other. The **lower node number is the initiator** (sends pings); the higher
one is the **responder** (replies only). This needs no extra config and halves airtime compared with both sides pinging.
Consequence: if only the higher-numbered board has its peer set, nothing is sent. Both boards must be configured.

## 3. Wire format (port 256 `PRIVATE_APP`, payload of `meshtastic_Data`)

Fixed little-endian layout. All packets share a 4-byte header.

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | magic | `0x54 0x48` ("TH"). Port 256 is shared: upstream `GamesModule` uses it when `BASEUI_HAS_GAMES=1` (off by default). Anything without the magic is ignored. |
| 2 | 1 | version | `1`. Unknown versions are ignored. |
| 3 | 1 | type | `1` = PING, `2` = REPLY |

**PING** (13 bytes total), initiator → responder:

| Off | Size | Field |
|---|---|---|
| 4 | 2 | `seq` (u16, 0..65534, wraps modulo 65535 so it never equals `0xFFFF`), increments only when a ping is actually handed to the router |
| 6 | 2 | `rpt_seq`: seq of the last REPLY the initiator heard (`0xFFFF` = none yet) |
| 8 | 2 | `rpt_rssi` (i16, dBm): initiator's RX RSSI of that reply |
| 10 | 1 | `rpt_snr_q4` (i8, SNR × 4, i.e. 0.25 dB steps; range ±31.75 dB) |
| 11 | 1 | `tx_power` (i8, dBm; `config.lora.tx_power`, 0 = region default) |
| 12 | 1 | `flags` (reserved, 0) |

**REPLY** (11 bytes total), responder → initiator:

| Off | Size | Field |
|---|---|---|
| 4 | 2 | `seq`: echoes the ping |
| 6 | 2 | `rssi` (i16): responder's RX RSSI of that ping |
| 8 | 1 | `snr_q4` (i8): responder's RX SNR of that ping |
| 9 | 1 | `tx_power` (i8) |
| 10 | 1 | `flags` (reserved, 0) |

So both boards learn both directions. The initiator gets "their RX of me" from each REPLY and measures "my RX of
them" on the REPLY itself. The responder measures each PING and gets "their RX of me" from `rpt_*`, one exchange late.

**Packet addressing.** Unicast to the peer, `hop_limit = 0`, `want_ack = false`, `want_response = false`:
- `hop_limit 0`: nobody relays it, same as `RangeTestModule`.
- No `want_response`: otherwise, if a reply is suppressed by the airtime gate, `callModules` would send a
  `NO_RESPONSE` NAK, which is extra traffic.
- Unicast means PKI encryption (`wouldEncryptWithPKC`, +12 bytes) and **no** XEdDSA signature. A broadcast would
  carry a 64-byte signature if the user set the signing policy to BALANCED/STRICT.
- Unicast needs the peer's public key (`Router.cpp` perhapsEncode refuses a "legacy DM" otherwise). Before each send:
  if PKI applies and `nodeDB->copyPublicKey(peer)` fails, skip the ping, log `TETHER idle reason=no_peer_key`, and
  at most every 5 minutes (gated like any send) unicast our NodeInfo to the peer with `want_response` so it
  answers with its own (NodeInfo is PKI-exempt). Normally keys are already known from boot-time NodeInfo.

**On-air size** (16-byte header + Data protobuf + 12 PKI): PING ≈ 48 B, REPLY ≈ 46 B. The firmware computes
real airtime with `RadioInterface::getPacketTime()`; the numbers below are from the Semtech formula
(16-symbol preamble, explicit header, CRC on, LDRO when symbol ≥ 16 ms).

## 4. Scheduling and airtime

**Gate on every send** (ping, reply, NodeInfo request): `airTime->isTxAllowedChannelUtil(true)` (polite, 25 %
channel busy) **and** `airTime->isTxAllowedAirUtil()` (our hourly TX duty cycle). `RangeTestModule` checks only the
first. `NeighborInfoModule` and the Telemetry modules check both, and so does TETHER. A gated send logs
`TETHER skip reason=chutil|airutil` and does **not** consume a sequence number.

**Interval** (initiator only): `interval = max(exchange_airtime / budget, min_interval)` with
`budget = 10 %` (`TETHER_AIRTIME_BUDGET_PCT`) and `min_interval = 2 s` (`TETHER_MIN_INTERVAL_MS`). There is no
maximum: the budget wins on slow presets. Recomputed every tick from the live preset, ±10 % jitter so it doesn't
lock step with other periodic traffic.

| Preset | SF/BW/CR | Ping | Reply | Exchange | Interval |
|---|---|---|---|---|---|
| SHORT_TURBO | 7/500/4:5 | 26 ms | 25 ms | 52 ms | 2.0 s (min) |
| SHORT_FAST | 7/250/4:5 | 53 ms | 50 ms | 103 ms | 2.0 s (min) |
| SHORT_SLOW | 8/250/4:5 | 95 ms | 90 ms | 186 ms | 2.0 s (min) |
| MEDIUM_FAST | 9/250/4:5 | 170 ms | 170 ms | 341 ms | 3.4 s |
| MEDIUM_SLOW | 10/250/4:5 | 321 ms | 321 ms | 641 ms | 6.4 s |
| **LONG_FAST** | 11/250/4:5 | 600 ms | 600 ms | 1.2 s | **12 s** |
| LONG_MODERATE | 11/125/4:8 | 1.9 s | 1.9 s | 3.8 s | 38 s |
| LONG_SLOW | 12/125/4:8 | 3.5 s | 3.5 s | 7.1 s | 71 s |

**Back-off.** After 3 consecutive pings with no reply, each further miss doubles the interval, up to 8×.
A reply resets it. A lost link must not keep the channel at full budget.

**Start delay.** First ping no earlier than `MESHMODULE_MIN_BROADCAST_DELAY_MS` (30 s) after boot.

## 5. Passive RSSI/SNR from normal traffic

`TetherSniffer` sets `isPromiscuous = true` and `encryptedOk = true`. It wants a packet when:
- `from == peer`
- the packet is **direct**: `hop_start != 0 && hop_limit == hop_start` (a relayed copy carries the relayer's
  signal, not the peer's)
- it is not a TETHER packet (port 256 when decoded)
- it has `rx_rssi`/`rx_snr` (radio-received, not local)

Encrypted packets (e.g. the peer's DMs to others) still carry `from`, hop fields and RX metrics, so they count too.
Always returns `CONTINUE`.

**Limit:** modules run in setup order and any earlier module returning `STOP` hides the packet.
TrafficManagement (when enabled) and StoreForward do this for some packets. Passive samples are best-effort and are
logged separately (`kind=passive`) from ping/reply samples.

## 6. Stats

- **Rings:** 128 samples each, `local` (my RX of peer) and `remote` (peer's RX of me).
  Sample = `{t_ms u32, seq u16, rssi i16, snr_q4 i8, kind u8}`, 10 B, so 2.5 KB total in RAM. Kinds: `ping`, `reply`, `passive`.
- **Aggregates per direction:** last value, EWMA (α = 0.25) of RSSI and SNR, min/max since boot.
- **Loss:**
  - The initiator counts `pings_sent` and `replies_heard`, giving round-trip delivery = replies / pings over the last 32
    pings. It cannot tell which leg failed, and a reply suppressed by the responder's gate looks like loss.
  - The responder counts sequence gaps in received pings, giving initiator→responder loss.
- **Fade margin**, two numbers, both against the **current preset**:
  - `margin_snr = snr − snr_floor(SF)`, with floors −7.5/−10/−12.5/−15/−17.5/−20 dB for SF7…12 (SX126x datasheet
    demodulator limits). Comparable across board revisions. Note LoRa SNR saturates near +10 dB, so on a strong
    link this underestimates.
  - `margin_rssi = rssi − sensitivity`, `sensitivity = −174 + 10·log10(BW_Hz) + NF + snr_floor(SF)`,
    `NF = 6 dB` (gives −137 dBm at SF12/BW125, matching the SX1262 datasheet). Uses absolute RSSI, so it is
    **not comparable** across V4.2 (GC1109) / V4.3 (KCT8103L) / V3. Use it as a relative trend on the same hardware.
- **Preset source:** `config.lora.use_preset` → `modemPresetToParams(preset, false, bw, sf, cr)` (the same call
  `Default.h` uses), otherwise `config.lora.spread_factor` / `bwCodeToKHz(config.lora.bandwidth)` / `coding_rate`.

## 7. Serial output

One `LOG_INFO` line per event, prefix `TETHER`, then space-separated `key=value`. The laptop logger (Phase 4b)
parses these. Phase 4 adds InfluxDB line protocol.

```
TETHER tx    kind=ping  peer=!1a2b3c4d seq=42 interval_ms=12000 airtime_ms=600
TETHER rx    kind=reply peer=!1a2b3c4d seq=42 rssi=-91 snr=6.25 margin_snr=23.75 margin_rssi=40.5
TETHER rpt   peer=!1a2b3c4d seq=42 rssi=-95 snr=4.50                # peer's RX of me
TETHER rx    kind=passive peer=!1a2b3c4d rssi=-90 snr=6.00 port=3
TETHER skip  reason=chutil ch_util=27.1 air_util_tx=1.2
TETHER idle  reason=no_peer
TETHER stats peer=!1a2b3c4d role=initiator sent=120 replies=111 pdr32=0.94 rssi_ewma=-92.1 snr_ewma=5.8 rpt_rssi_ewma=-95.3 rpt_snr_ewma=4.1 backoff=1
```
`stats` every 10 pings or 60 s, whichever comes first.

Added after Phase 1 (compiled, not run on hardware):
```
TETHER rx kind=reply ... lat=39.739200 lon=-104.990300 alt=1609 hdop=0.90 sats=11 fix=3 gps_t=1762412431 gps=ok
TETHER rx kind=reply ... gps=hdop sats=7 hdop=3.10                  # fix failed the quality gate: no position
TETHER power vbat_mv=3950 vbat_ewma=3948 pct=81.5 usb=0 charging=0 trend_pct_h=-6.00 runtime_h=13.6 mult=1
TETHER power_mode mult=2 pct=19 charging=0                          # low-battery slow-down changed
```
- GPS suffix on `rx` and `rpt` lines (`TETHER_GPS_TAG`): smoothed local position from `gpsStatus`, HDOP/fix type/time
  from `localPosition`. Gate: lock, >= 6 satellites, HDOP <= 2.0 (`TetherGeo.h`).
- `power` every 60 s from `powerStatus` (Meshtastic's LiPo OCV curve gives `pct`); trend and runtime from
  `TetherPower.h`. `mult` multiplies the ping interval (x2 below 20 %, x4 below 10 %, not while charging).
- OLED page: see [OLED.md](OLED.md).

Meshtastic prints debug logs on the USB serial port while no API client is attached. With a client attached they
arrive as `LogRecord`s only if `security.debug_log_api_enabled` is on. The logger will rely on that.

## 8. Requirements and known limits

- `device.rebroadcast_mode` must **not** be `CORE_PORTNUMS_ONLY`, because `Router::dispatchReceived` drops non-core ports
  (including 256) before modules see them. TETHER logs a warning at boot if it is set.
- Both boards on the same channel/preset/region; region must be set.
- The stock phone app shows nothing from port 256. All output goes to serial (later OLED/MQTT/PWA).
- RAM-only stats. Nothing persists across reboot.

## 9. Testing

1. **Host unit tests** (no hardware, `python scripts/run_host_tests.py`, also in CI): `TetherProtocol.h`,
   `TetherMath.h`, `TetherStats.h` (160 checks) and `TetherCanvas.h`, `TetherScreen.h`, `TetherGeo.h`, `TetherPower.h`
   (73 checks). Clean under clang and GCC 14 with `-Wall -Wextra -Werror`.
2. **Compile** `heltec-v4` with the module installed (`python scripts/build_firmware.py`); flash/RAM deltas in
   [BUILD.md](BUILD.md). Done, no warnings from TETHER sources.
3. **Hardware** (needs the two boards): [HARDWARE_TEST.md](HARDWARE_TEST.md). Not done yet. `TetherModule.cpp` (the
   Meshtastic-facing glue: scheduling, send/receive, sniffer, OLED blit, GPS/power reads) is only covered here.
