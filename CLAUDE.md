# SmartFires IoT — Claude Context

## Project Purpose

Wildfire IoT sensor network. Remote drone nodes collect environmental data (temperature, humidity, wind speed, particulate matter, GPS) and transmit it wirelessly over LoRa to a base station connected to a Jetson Orin Nano edge computer.

---

## Hardware Topology

```
[ESP32 drone node]
    sensors: wind, SHT31 (temp/humidity), GPS (PA1010D),
             SPS30 (PM1.0/PM2.5/PM4.0/PM10), ICM-20948 IMU, OLED display
    |
    | UART 115200 baud (Serial1, binary frames — bidirectional)
    |   ESP32 → Feather: BUNDLE frames (≤145 bytes, 4 Hz sensing, 7 deltas per bundle)
    |                    GPS frame (16 bytes, sent once per session on first valid fix)
    |   Feather → ESP32: ACK text lines + TIME_SYNC binary frames (16 bytes)
    v
[Adafruit Feather M0 RFM95 — node side]
    TX queue (4 slots, drop-oldest) decouples ESP32 sensing rate from LoRa TX rate.
    TDMA: transmits only in its assigned time slot (derived from session_time).
    CAD check before each TX (10 ms channel sense) to detect collisions.
    |
    | LoRa 915 MHz (RadioHead RHReliableDatagram, 13 dBm)
    |   Node → Base: BUNDLE payload (≤141 bytes, 1 retry, 100 ms timeout, TDMA-gated)
    |                GPS payload (12 bytes, once per session)
    |   Base → Node: TIME_SYNC broadcast (12 bytes, fire-and-forget, RH_BROADCAST_ADDRESS)
    v
[Adafruit Feather M0 RFM95 — base station side]
    Receives telemetry, auto-ACKs, relays to Jetson.
    Reads TIME_SYNC commands from Jetson UART, broadcasts them over LoRa.
    |
    | UART 115200 baud (Serial1, binary frames — bidirectional)
    |   Feather → Jetson: base UART frames with RSSI (variable length, ≤146 bytes)
    |   Jetson → Feather: TIME_SYNC command frames (16 bytes, every 30 s)
    v
[Jetson Orin Nano]
    edge/receiver.py → telemetry.csv
    Sends periodic TIME_SYNC frames to keep all nodes on a common session clock.
```

For multiple nodes, each node has its own ESP32 + Feather M0 pair. All nodes share
the 915 MHz channel and are collision-free via TDMA slot assignment.

---

## Repository Layout

```
SmartFires_IoT/
├── platformio/               PlatformIO project (all embedded firmware)
│   ├── platformio.ini        Build environments — see table below
│   ├── src/
│   │   ├── shared/           Headers included by all targets
│   │   │   ├── BinaryPacket.h      Wire format: structs, CRC, frame encoders ← main protocol file
│   │   │   ├── TelemetryPacket.h   Internal ESP32 sensor data struct (floats)
│   │   │   ├── TelemetryCodec.h    Legacy text/CSV codec (no longer used on wire)
│   │   │   └── UartLoRaBridge.h    UART bridge helper used by drone/main.cpp
│   │   ├── drone/            ESP32 firmware (sensors + telemetry sender)
│   │   └── lora_feather/     Feather M0 firmware (NODE and BASE modes, one file)
│   └── include/              Sensor/actuator driver headers (ESP32 targets only)
│       ├── ISensor.h / IActuator.h / II2CDevice.h   Interfaces
│       ├── FlameSensor.h / WindSensorRevC.h          Header-only drivers
│       ├── Sht31Sensor.h / Pa1010dGpsSensor.h        (impls in src/drone/)
│       ├── Icm20948Imu.h / LidarLiteV3.h             (LidarLiteV3 impl in src/drone/)
│       ├── OledDisplay.h / MicroServo.h / PassiveBuzzer.h
│       └── PinMapping.h      ESP32 pin assignments
├── edge/                     Jetson Python code (not part of PlatformIO)
│   ├── receiver.py           UART frame receiver → CSV writer + TIME_SYNC sender
│   ├── packet.py             Python mirror of BinaryPacket.h (encode + decode)
│   └── requirements.txt      pyserial>=3.5
├── lora/                     Legacy experimental LoRa sketches (ignore)
└── platformio/TELEMETRY_REWORK_PLAN.md   Full protocol design doc
```

---

## Build Environments

| Environment | Board | Key flags | Purpose |
|---|---|---|---|
| `drone` | Arduino Nano ESP32 | `NODE_ID=1` | Sensor node firmware |
| `drone_node2` | Arduino Nano ESP32 | `NODE_ID=2` | Sensor node firmware — node 2 |
| `lora_feather` | Feather M0 | `LORA_NODE=1` `NODE_ID=1` `NUM_SLOTS=2` | Radio node side — node 1 |
| `lora_feather_node2` | Feather M0 | `LORA_NODE=1` `NODE_ID=2` `NUM_SLOTS=2` | Radio node side — node 2 |
| `lora_feather_base` | Feather M0 | `LORA_BASE_STATION=1` | Radio base station side |
| `uno` | Arduino Uno | — | Legacy, unused |

### Adding a node

1. Duplicate `lora_feather` and `drone` environments in `platformio.ini`.
2. Set `NODE_ID=N` in both new environments.
3. **Update `NUM_SLOTS` to the new total node count in ALL `lora_feather*` environments.**
4. Reflash every node Feather — they all need the same `NUM_SLOTS` for TDMA to work.

`NUM_SLOTS` is the only flag that must match across all node Feathers. `NODE_ID` is unique per device.

### Common build commands (run from `platformio/`)

```bash
pio run -e drone               --target upload   # flash ESP32
pio run -e lora_feather        --target upload   # flash node Feather
pio run -e lora_feather_base   --target upload   # flash base Feather
pio device monitor -e drone                      # serial monitor ESP32
```

---

## Wire Protocol

### UART frames — drone → feather node (variable)

```
BUNDLE:     [0xAA][0x55][len: u8][PktHeader: 4][FullStatePayload: 24][n_deltas: 1][DeltaPayload×n: n×16][crc8]
            len = 29 + n×16  (max n=7 → len=141, frame=145 bytes)
GPS:        [0xAA][0x55][len=12: u8][PktHeader: 4][GpsPayload: 8][crc8]
            Sent once per session on first valid GPS fix. frame=16 bytes.
```

### LoRa payloads — feather node → feather base (variable)

```
BUNDLE:  [PktHeader: 4][FullStatePayload: 24][n_deltas: 1][DeltaPayload×n: n×16]  ≤ 141 bytes
GPS:     [PktHeader: 4][GpsPayload: 8]                                               = 12 bytes
```

RadioHead `RHReliableDatagram` handles LoRa framing, ACK, and retry. The node calls
`sendtoWait()` (1 retry, 100 ms timeout per attempt); the base calls `recvfromAck()`
which auto-sends the ACK. The raw sensor payload is forwarded as-is — no re-encode.
A LoRa CAD check (10 ms) runs before each TX to detect channel activity.

### LoRa TIME_SYNC broadcast — feather base → all nodes (12 bytes)

```
[PktHeader: 4 bytes][TimeSyncPayload: 8 bytes]
```

Sent to `RH_BROADCAST_ADDRESS (0xFF)` — fire-and-forget, no ACK. Nodes that are listening
(between their own TX slots) receive this and update their TDMA session clock.

**RHReliableDatagram address scheme:**

| Device | Address |
|---|---|
| Base station | `0x01` (`BASE_ADDR`) |
| Node 1 | `0x01` (= `NODE_ID`) |
| Node N | `NODE_ID` |
| Broadcast | `0xFF` (`RH_BROADCAST_ADDRESS`) — used for TIME_SYNC only |

Note: with the current single-node setup NODE_ID=1 and BASE_ADDR=0x01 share the same
value. When adding node 2, set `NODE_ID=2` for that Feather — its address becomes 0x02.

### UART frame — feather base → Jetson (variable length)

```
[0xAA][0x55][len: u8][rssi: i8][LoRa payload][crc8]
len = 1 (rssi) + lora_payload_len
  GPS:    len=13  (rssi + PktHeader:4 + GpsPayload:8)
  BUNDLE: len=142 max  (rssi + PktHeader:4 + FullStatePayload:24 + n_deltas:1 + 7×DeltaPayload:112)
```

### UART TIME_SYNC frame — Jetson → feather base  OR  feather node → ESP32 (16 bytes)

```
[0xAA][0x55][len=12: u8][PktHeader(PKT_TIME_SYNC): 4 bytes][TimeSyncPayload: 8 bytes][crc8]
```

Same frame format in both directions. The base Feather re-encodes the Jetson command into a
12-byte LoRa broadcast. The node Feather re-encodes the LoRa broadcast back into this 16-byte
UART frame before forwarding to the ESP32.

### PktHeader (4 bytes, packed)

| Field | Type | Notes |
|---|---|---|
| `magic` | `uint8_t` | `0xA5` |
| `pkt_type` | `uint8_t` | `0x01` FULL_STATE · `0x02` HEARTBEAT · `0x03` TIME_SYNC · `0x04` BUNDLE · `0x05` GPS |
| `node_id` | `uint8_t` | compile-time `NODE_ID`; `0` for TIME_SYNC frames |
| `seq` | `uint8_t` | rolling 0–255 |

### FullStatePayload (24 bytes, packed, little-endian)

GPS coordinates removed — transmitted once per session via `PKT_GPS` to save 8 bytes per packet.

| Field | Type | Encoding |
|---|---|---|
| `session_time` | `uint32_t` | ms since Jetson session start (synced via TIME_SYNC; falls back to local millis() until first sync) |
| `uptime_ms` | `uint32_t` | ESP32 local millis() — used for sync diagnostics and delta dt_ms computation |
| `sensor_flags` | `uint16_t` | bitmask: WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10 |
| `wind_cms` | `uint16_t` | cm/s = windMps × 100 |
| `temp_cdegc` | `int16_t` | centi-°C = tempC × 100 |
| `humidity_cpct` | `uint16_t` | centi-% = humidityPct × 100 |
| `pm1_0_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm2_5_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm4_0_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm10_ug10` | `uint16_t` | µg/m³ × 10 |

> **TODO:** Force a TIME_SYNC round-trip at startup before the ESP32 begins sending
> telemetry. Once guaranteed, `uptime_ms` becomes redundant with `session_time` and can
> be dropped, saving 4 bytes per packet (recalculate slot sizing when removing it).

### GpsPayload (8 bytes, packed, little-endian)

Sent once per session via `PKT_GPS` when the ESP32 first acquires a valid GPS fix.
A new `session_id` (receiver.py restart) causes the firmware to re-send GPS on the next fix.

| Field | Type | Encoding |
|---|---|---|
| `lat_e7` | `int32_t` | degrees × 1e7 |
| `lon_e7` | `int32_t` | degrees × 1e7 |

### DeltaPayload (16 bytes, packed, little-endian)

Compact representation of one sample relative to the bundle's reference `FullStatePayload`.
`wind_cms` is absolute (not a delta) because wind can change rapidly. All other fields are signed deltas.

| Field | Type | Encoding |
|---|---|---|
| `dt_ms` | `uint16_t` | ms elapsed since reference `session_time` |
| `wind_cms` | `uint16_t` | absolute wind speed in cm/s |
| `temp_delta_cdegc` | `int16_t` | delta from reference in centi-°C |
| `humidity_delta_cpct` | `int16_t` | delta from reference in centi-% |
| `pm1_0_delta` | `int16_t` | delta in µg/m³ × 10 |
| `pm2_5_delta` | `int16_t` | delta in µg/m³ × 10 |
| `pm4_0_delta` | `int16_t` | delta in µg/m³ × 10 |
| `pm10_delta` | `int16_t` | delta in µg/m³ × 10 |

### TimeSyncPayload (8 bytes, packed, little-endian)

| Field | Type | Notes |
|---|---|---|
| `session_id` | `uint32_t` | Random value set at receiver.py startup. Changes on Jetson restart so nodes know to reset their offset. |
| `session_time_ms` | `uint32_t` | ms since receiver.py started. Wraps at ~49 days. |

CRC: CRC-8/MAXIM (polynomial 0x31), covers the `len` byte + all data bytes. Same algorithm
in C++ (`BinaryPacket::crc8`) and Python (`packet.crc8`).

---

## TDMA — Time Division Multiple Access

### Overview

All nodes share the 915 MHz LoRa channel. TDMA divides time into repeating frames, each
split into `NUM_SLOTS` equal slots. Each node transmits only in its assigned slot, eliminating
collisions without any runtime negotiation.

```
|<-------------- kFramePeriodMs = NUM_SLOTS × kSlotWidthMs ------------->|
|  slot 0 (node 1)  |  slot 1 (node 2)  |  slot 2 (node 3)  |  ...      |
|--guard--|--TX win--|--guard--|--TX win--|--guard--|--TX win--|
    20 ms     860 ms     20 ms     860 ms     20 ms     860 ms
```

### Slot assignment — compile-time, not runtime

**Slot index = `(NODE_ID - 1) % NUM_SLOTS`**

| `NODE_ID` | Slot | Active TX window in a 1800 ms frame (NUM_SLOTS=2) |
|---|---|---|
| 1 | 0 | session_ms % 1800 in [20, 880) |
| 2 | 1 | session_ms % 1800 in [920, 1780) |

There is no discovery protocol. The slot is baked into the firmware at flash time via the
`NODE_ID` and `NUM_SLOTS` build flags. The base station and Jetson are completely uninvolved.

**Operational rule:** when adding a node, update `NUM_SLOTS` in **every** `lora_feather*`
environment and reflash all node Feathers. Mismatched `NUM_SLOTS` will cause nodes to
collide or transmit in each other's slots.

### TDMA parameters

| Parameter | Value | Rationale |
|---|---|---|
| `kSlotWidthMs` | 900 ms | Worst-case 1 retry × 2 attempts × (236 ms TX + 100 ms timeout) + 2×20 ms guard = 712 ms < 860 ms active window |
| `kGuardMs` | 20 ms | Covers Feather M0 crystal drift (≤1.5 ms per 30 s sync interval at 50 ppm) plus processing jitter |
| `LORA_RETRIES` | 1 | One re-attempt after first TX; ACK typically arrives in ~35 ms; timeout of 100 ms gives headroom |
| `LORA_TIMEOUT_MS` | 100 ms | Per-attempt ACK timeout; kept short so retries still fit inside the slot |
| `NUM_SLOTS` (build flag) | 2 (default) | Match to physical node count; reflash all Feathers when changed |
| `kSyncStaleMs` | 300 000 ms | If no TIME_SYNC for 5 min, revert to immediate TX (prevents silent failure if Jetson dies) |

### Session clock and drift

The TDMA clock on each node Feather is:

```
session_time_now = tdmaSyncSessionMs + (millis() - tdmaSyncLocalMs)
```

where `tdmaSyncSessionMs` and `tdmaSyncLocalMs` are updated on every TIME_SYNC broadcast.
The Feather M0's crystal is ±25–50 ppm → ≤1.5 ms drift per 30 s sync interval,
well within the 20 ms guard band. All nodes see the same LoRa propagation delay from
the base (~50 ms for 12 bytes at SF7) so relative slot alignment is unaffected.

### TX queue and packet freshness

The node Feather holds a 4-slot ring buffer of LoRa payloads. When the buffer is full,
the **oldest** entry is evicted to make room for the newest sensor data. This ensures
the queue always holds the most recent readings. At 4 Hz sensing (N_δ=7), a bundle
accumulates every 2 s; with 2 nodes at 900 ms slots, T_frame=1.8 s → η=0.90 (no data loss).

### Stale-sync fallback

If no TIME_SYNC is received for more than `kSyncStaleMs` (5 min), TDMA is suspended
and the node reverts to immediate TX on every `drainTxQueue()` call. This prevents
total silence if the Jetson crashes or the base Feather loses power.

---

## TIME_SYNC flow

```
receiver.py (Jetson)
  │  Sends 16-byte TIME_SYNC UART frame at startup and every 30 s
  │  session_id = random uint32 set at startup (changes on restart)
  │  session_time_ms = ms since receiver.py started
  ▼
Base Feather (Serial1 RX)
  │  Parses frame, extracts TimeSyncPayload
  │  Broadcasts 12-byte LoRa packet to RH_BROADCAST_ADDRESS
  ▼
Node Feather (LoRa RX, checked in checkIncomingLora() after each TX)
  │  Decodes TimeSyncPayload
  │  Updates tdmaSyncSessionMs + tdmaSyncLocalMs (Feather TDMA clock)
  │  Re-encodes as 16-byte UART frame → sends to ESP32 via Serial1
  ▼
ESP32 (UartLoRaBridge binary parser)
  │  Detects 0xAA at start of message → binary frame mode
  │  Decodes TimeSyncPayload
  │  Updates _sessionTimeOffset = sessionMs - millis()
  ▼
LinkService::sendTelemetryFrame()
  session_time = millis() + _sessionTimeOffset   ← attached to every packet
```

**Timestamp accuracy:** ≤50–100 ms absolute (dominated by LoRa TX time of the TIME_SYNC broadcast).
Relative accuracy between nodes on the same sync: <2 ms (all nodes see the same propagation delay).

---

## Telemetry Rework Phases

Defined in full at `platformio/TELEMETRY_REWORK_PLAN.md`.

| Phase | Status | Description |
|---|---|---|
| 1 — Binary full-state | **Done** | Replaced text CSV with packed binary frames. |
| 1b — LoRa ACK/retry | **Done** | `RHReliableDatagram`. Node: 1 retry, 100 ms timeout. Base: auto-ACK via `recvfromAck()`. CAD check before TX. |
| 1c — Feather TX queue | **Done** | 4-slot drop-oldest ring buffer on node Feather. Decouples ESP32 sensing rate from LoRa TX rate. |
| 2 — TIME_SYNC | **Done** | Jetson → base UART → LoRa broadcast → node → ESP32. `session_time` in every packet reflects Jetson wall clock. 30 s sync interval, <2 ms relative drift between nodes. Serial debug prints at every hop. |
| 2b — TDMA | **Done** | Compile-time slot assignment via `NODE_ID` and `NUM_SLOTS`. 900 ms slots, 20 ms guard, stale-sync fallback. Eliminates multi-node collisions without GPS or runtime negotiation. |
| 3 — Delta packets | **Done** | `PKT_BUNDLE`: reference `FullStatePayload` + up to 7 `DeltaPayload` entries. GPS separated into one-time `PKT_GPS` per session (saves 8 bytes per packet). 4 Hz sensing, bundle every 2 s at N_δ=7. |
| 4 — Feather duty cycle | Not started | Sleep radio between TX slots; ESP32 batches between wakeups. |
| 5 — Receiver recovery | Not started | Sequence gap handling, SYNC_REQUEST, debug counters. |

---

## Edge Receiver (Jetson)

```bash
# Install dependency
pip install -r edge/requirements.txt

# Run (adjust --port after jetson-io enables the UART)
python3 edge/receiver.py --output telemetry.csv   # default port /dev/ttyTHS1, sync every 30 s
python3 edge/receiver.py --sync-interval 10       # faster sync (useful during initial testing)
```

The receiver is **bidirectional**:
- Reads variable-length telemetry frames (≤146 bytes) from the base Feather → writes CSV rows
- Sends 16-byte TIME_SYNC frames to the base Feather at startup and every `--sync-interval` seconds

Each receiver.py restart generates a new random `session_id`. Nodes detect the change and
reset their `session_time` offset automatically.

Jetson UART setup (one-time):
1. `sudo /opt/nvidia/jetson-io/jetson-io.py` — enable the UART pin group
2. `sudo systemctl disable nvgetty && sudo udevadm trigger` — free the port from the serial console
3. Typical device path: `/dev/ttyTHS1` (confirmed working on this Jetson)

CSV columns: `timestamp` (Jetson UTC wall clock at receipt), `node_id`, `seq`, `session_time_ms`
(ESP32 synced session clock), `uptime_ms`, `sensor_flags`, `wind_mps`, `temp_c`, `humidity_pct`,
`pm1_0_ug_m3`, `pm2_5_ug_m3`, `pm4_0_ug_m3`, `pm10_ug_m3`, `lat`, `lon`, `rssi`

---

## Key Design Decisions

- **Binary not text:** text CSV was ~90 bytes/packet; binary bundle is ≤141 bytes over LoRa but carries 8 samples (reference + 7 deltas), so effective per-sample cost is ~18 bytes vs ~90 bytes.
- **Fixed-point integers:** no floats on the wire — deterministic size, no locale/printf issues on MCUs.
- **8-bit rolling seq in header:** sufficient for drop detection; full `uint32_t` seq lives only in ESP32 RAM. ACK comparison uses `& 0xFF` on both sides.
- **TDMA slot assignment is compile-time:** `kMySlot = (NODE_ID - 1) % NUM_SLOTS`. No runtime negotiation, no discovery packets, no coordinator needed. Adding a node = change `NUM_SLOTS`, reflash all Feathers.
- **`NUM_SLOTS` must match across all node Feathers.** A mismatch causes slot collisions. The base station and ESP32 are completely unaware of TDMA — only the node Feathers enforce it.
- **Feather owns TX timing; ESP32 owns sensing rate.** The ESP32 enqueues at 1 Hz regardless of TDMA. The Feather's drop-oldest queue ensures freshness. They are fully decoupled.
- **TIME_SYNC driven by Jetson, not GPS.** The Jetson's NTP clock is the authority. GPS-based PPS sync is deferred — GPS TDMA will need <1 ms accuracy (achievable with PPS); current accuracy is ≤1.5 ms relative between synced nodes, which is sufficient for the current slot widths.
- **Node Feather maintains its own TDMA clock independently of the ESP32.** The Feather tracks `tdmaSyncSessionMs + elapsed millis()` locally. The ESP32 gets a separate TIME_SYNC forwarded over UART for packet timestamping. These are independent paths.
- **Stale-sync fallback to immediate TX:** if no TIME_SYNC for 5 min the node transmits unthrottled (potential multi-node collisions) rather than going silent. Wildfire monitoring prioritises data delivery over collision avoidance.
- **LoRa ACK/retry via RHReliableDatagram:** node uses `sendtoWait()` (1 retry, 100 ms timeout). Base uses `recvfromAck()` which auto-ACKs. TIME_SYNC is broadcast (`RH_BROADCAST_ADDRESS`) — no ACK, no retry; nodes receive it opportunistically between their own TX slots.
- **Raw LoRa payload forwarding:** Feather node sends the sensor payload bytes verbatim; no re-encode. Jetson owns parsing.
- **RSSI in base frame:** prepended as `int8_t` before the LoRa payload. Logged to CSV for link quality analysis.
- **CRC-8/MAXIM (0x31):** standard single-byte CRC, same algorithm in C++ and Python.
- **`TelemetryCodec.h` retained:** still compiles but no longer used on the wire. Can be removed once Phase 3 is underway.
