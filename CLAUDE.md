# SmartFires IoT — Claude Context

## Project Purpose

Wildfire IoT sensor network. Remote drone nodes collect environmental data (temperature, humidity, wind speed, flame detection, GPS, LIDAR) and transmit it wirelessly over LoRa to a base station connected to a Jetson Orin Nano edge computer.

---

## Hardware Topology

```
[ESP32 drone node]
    sensors: wind, flame, SHT31 (temp/humidity), GPS (PA1010D),
             LIDAR Lite v3, ICM-20948 IMU, OLED display
    |
    | UART 115200 baud (Serial1, binary frames)
    v
[Adafruit Feather M0 RFM95 — node side]
    |
    | LoRa 915 MHz (RadioHead RH_RF95, 13 dBm)
    v
[Adafruit Feather M0 RFM95 — base station side]
    |
    | UART 115200 baud (Serial1, binary frames with RSSI)
    v
[Jetson Orin Nano]
    edge/receiver.py → telemetry.csv
```

For multiple nodes, each node has its own ESP32 + Feather M0 pair. All nodes share the same 915 MHz channel. Collision avoidance: staggered TX intervals with jitter (Phase 1 approach); TDMA is planned for Phase 2 once TIME_SYNC is live.

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
│   ├── receiver.py           UART frame receiver → CSV writer
│   ├── packet.py             Python mirror of BinaryPacket.h (decode only)
│   └── requirements.txt      pyserial>=3.5
├── lora/                     Legacy experimental LoRa sketches (ignore)
└── platformio/TELEMETRY_REWORK_PLAN.md   Full protocol design doc
```

---

## Build Environments

| Environment | Board | Key flags | Purpose |
|---|---|---|---|
| `drone` | Arduino Nano ESP32 | `NODE_ID=1` | Sensor node firmware |
| `lora_feather` | Feather M0 | `LORA_NODE=1` `NODE_ID=1` | Radio node side |
| `lora_feather_base` | Feather M0 | `LORA_BASE_STATION=1` | Radio base station side |
| `uno` | Arduino Uno | — | Legacy, unused |

**Adding a second node:** duplicate `lora_feather` and the `drone` envs in `platformio.ini`, set `NODE_ID=2` in both. NODE_ID is the only thing that differentiates nodes.

### Common build commands (run from `platformio/`)

```bash
pio run -e drone               --target upload   # flash ESP32
pio run -e lora_feather        --target upload   # flash node Feather
pio run -e lora_feather_base   --target upload   # flash base Feather
pio device monitor -e drone                      # serial monitor ESP32
```

---

## Wire Protocol

### UART frame — drone → feather node (35 bytes)

```
[0xAA][0x55][len=31: u8][PktHeader: 4 bytes][FullStatePayload: 27 bytes][crc8]
```

### LoRa payload — feather node → feather base (31 bytes)

```
[PktHeader: 4 bytes][FullStatePayload: 27 bytes]
```

RadioHead `RHReliableDatagram` handles LoRa framing, ACK, and retry. The node calls
`sendtoWait()` (up to 5 retries, 300 ms timeout per attempt); the base calls `recvfromAck()`
which auto-sends the ACK. The raw sensor payload is forwarded as-is — no re-encode.

**RHReliableDatagram address scheme:**

| Device | Address |
|---|---|
| Base station | `0x01` (`BASE_ADDR`) |
| Node 1 | `0x01` (= `NODE_ID`) |
| Node N | `NODE_ID` |

Note: with the current single-node setup NODE_ID=1 and BASE_ADDR=0x01 share the same
value. When adding node 2, set `NODE_ID=2` for that Feather — its address becomes 0x02.

### UART frame — feather base → Jetson (36 bytes)

```
[0xAA][0x55][len=32: u8][rssi: i8][PktHeader: 4 bytes][FullStatePayload: 27 bytes][crc8]
```

### PktHeader (4 bytes, packed)

| Field | Type | Value |
|---|---|---|
| `magic` | `uint8_t` | `0xA5` |
| `pkt_type` | `uint8_t` | `0x01` = FULL_STATE |
| `node_id` | `uint8_t` | compile-time `NODE_ID` |
| `seq` | `uint8_t` | rolling 0–255 |

### FullStatePayload (27 bytes, packed, little-endian)

| Field | Type | Encoding |
|---|---|---|
| `session_time` | `uint32_t` | ms (local millis until TIME_SYNC) |
| `uptime_ms` | `uint32_t` | ms |
| `sensor_flags` | `uint16_t` | bitmask: FLAME=0x01 WIND=0x02 SHT31=0x04 LIDAR=0x08 GPS=0x10 IMU=0x20 |
| `flame` | `uint8_t` | 0 or 1 |
| `wind_cms` | `uint16_t` | cm/s = windMps × 100 |
| `temp_cdegc` | `int16_t` | centi-°C = tempC × 100 |
| `humidity_cpct` | `uint16_t` | centi-% = humidityPct × 100 |
| `lidar_cm` | `uint16_t` | cm |
| `lat_e7` | `int32_t` | degrees × 1e7 |
| `lon_e7` | `int32_t` | degrees × 1e7 |

CRC: CRC-8/MAXIM (polynomial 0x31), covers the `len` byte + all data bytes.

---

## Telemetry Rework Phases

Defined in full at `platformio/TELEMETRY_REWORK_PLAN.md`.

| Phase | Status | Description |
|---|---|---|
| 1 — Binary full-state | **Done** | Replace text CSV with packed binary frames. `session_time` uses local `millis()` as a placeholder. |
| 1b — LoRa ACK/retry | **Done** | Switched from raw `RH_RF95` to `RHReliableDatagram`. Node retries up to 5× (300 ms/attempt) before dropping. Base auto-ACKs via `recvfromAck()`. UART ACK to ESP32 is unchanged. |
| 2 — TIME_SYNC | Not started | Base station broadcasts authoritative session clock. Nodes maintain a `millis()` offset. `PKT_TIME_SYNC = 0x03` type already reserved in `BinaryPacket.h`. |
| 3 — Delta packets | Not started | Changed-field bitmask, threshold suppression, periodic full refresh. |
| 4 — Feather TX queue + duty cycle | Not started | Ring buffer on Feather node; priority classes; stale delta replacement. ESP32 batches packets, Feather wakes LoRa TX, sends batch with per-packet ACK/retry, sleeps radio to save power. Batch size is dynamic (sensing interval may vary); ESP32 may send metadata packets to Feather to signal batch start/size. |
| 5 — Receiver recovery | Not started | Sequence gap handling, SYNC_REQUEST, debug counters. |

---

## Edge Receiver (Jetson)

```bash
# Install dependency
pip install -r edge/requirements.txt

# Run (adjust --port after jetson-io enables the UART)
python3 edge/receiver.py --output telemetry.csv   # default port is /dev/ttyTHS1
```

Jetson UART setup (one-time):
1. `sudo /opt/nvidia/jetson-io/jetson-io.py` — enable the UART pin group
2. `sudo systemctl disable nvgetty && sudo udevadm trigger` — free the port from the serial console
3. Typical device path: `/dev/ttyTHS1` (confirmed working on this Jetson)

CSV columns: `timestamp, node_id, seq, session_time_ms, uptime_ms, sensor_flags, flame, wind_mps, temp_c, humidity_pct, lidar_cm, lat, lon, rssi`

---

## Key Design Decisions

- **Binary not text:** text CSV was ~90 bytes/packet; binary full-state is 31 bytes over LoRa.
- **Fixed-point integers:** no floats on the wire — deterministic size, no locale/printf issues on MCUs.
- **8-bit rolling seq in header:** sufficient for drop detection; full `uint32_t` seq lives only in ESP32 RAM. ACK comparison uses `& 0xFF` on both sides.
- **LoRa ACK/retry via RHReliableDatagram:** Node uses `sendtoWait()` (5 retries, 300 ms timeout each). Base uses `recvfromAck()` which auto-ACKs. Eliminates the ~40% fire-and-forget packet loss of the original raw `RH_RF95` approach.
- **Raw LoRa payload forwarding:** Feather node sends the sensor payload bytes verbatim; no re-encode. Jetson owns parsing.
- **RSSI in base frame:** prepended as `int8_t` before the LoRa payload. Logged to CSV for link quality analysis.
- **CRC-8/MAXIM (0x31):** standard single-byte CRC, good for short frames, same algorithm on both C++ and Python sides.
- **Two-node collision strategy (Phase 1):** staggered fixed TX intervals + random jitter. TDMA requires TIME_SYNC (Phase 2).
- **`TelemetryCodec.h` retained:** still compiles but no longer used on the wire. Can be removed once Phase 2 is underway.
