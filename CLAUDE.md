# SmartFires IoT — Claude Context

## Project Purpose

Wildfire IoT sensor network. Remote drone nodes collect environmental data (temperature, humidity, wind speed, particulate matter, GPS) and transmit it wirelessly over LoRa to a base station connected to a Jetson Orin Nano edge computer.

---

## Hardware Topology

```
[Adafruit Feather M0 RFM95 — node]
    sensors wired directly: SHT31 (temp/humidity), wind, GPS (PA1010D),
                            SPS30 (PM1.0/PM2.5/PM4.0/PM10), ICM-20948 IMU
    runs: SmartFiresNodeApp → PacketHandler → TdmaRadioService → RadioHeadTdmaDriver
    |
    | LoRa 915 MHz (RadioHead RHReliableDatagram, 13 dBm)
    |   Node → Base: BUNDLE payload (≤141 bytes, 1 retry, 100 ms timeout, TDMA-gated)
    |                GPS payload (12 bytes, once per session on first valid fix)
    |   Base → Node: TIME_SYNC broadcast (12 bytes, fire-and-forget, RH_BROADCAST_ADDRESS)
    v
[Adafruit Feather M0 RFM95 — base station]  ← NOT YET PORTED to class architecture
    Receives telemetry, auto-ACKs, relays to Jetson over UART.
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

Each node is a single Feather M0 with sensors directly attached — there is no separate
ESP32. The old two-MCU design (ESP32 + Feather M0 per node connected by UART) has been
replaced by this single-board architecture.

For multiple nodes, each node is its own Feather M0 unit. All share the 915 MHz channel
and are collision-free via TDMA slot assignment.

---

## Repository Layout

```
SmartFires_IoT/
├── platformio/                    PlatformIO project (all embedded firmware)
│   ├── platformio.ini             Build environments — see table below
│   ├── include/
│   │   ├── app/
│   │   │   └── SmartFiresNodeApp.h       Top-level application class
│   │   ├── interfaces/
│   │   │   ├── IClock.h                  millis() abstraction
│   │   │   ├── ISensor.h                 Sensor interface + fillSnapshot()
│   │   │   ├── IRadio.h
│   │   │   └── IAnalogReader.h
│   │   ├── radio/
│   │   │   ├── PacketHandler.h           Bundle + GPS packet assembly ← main protocol entry
│   │   │   ├── TdmaRadioService.h        TDMA-gated LoRa TX/RX orchestration
│   │   │   ├── TdmaClock.h               Session clock + slot timing
│   │   │   ├── TdmaConfig.h              All TDMA parameters in one struct
│   │   │   ├── TdmaTxQueue.h             4-slot drop-oldest ring buffer
│   │   │   └── ITdmaRadioDriver.h        Hardware abstraction for radio
│   │   ├── telemetry/
│   │   │   ├── BinaryPacket.h            Wire format: structs, CRC, encoders ← main protocol file
│   │   │   └── SensorSnapshot.h          Internal float-unit sensor reading struct
│   │   ├── sensors/
│   │   │   ├── Sht31Sensor.h             (impl in src/sensors/)
│   │   │   ├── WindSensorRevC.h
│   │   │   ├── Pa1010dGpsSensor.h
│   │   │   ├── Sps30Sensor.h
│   │   │   └── Icm20948Sensor.h
│   │   ├── drivers/                      Hardware driver interfaces (I2C, SPI)
│   │   │   ├── ISht31Driver.h
│   │   │   ├── IGpsDriver.h
│   │   │   ├── ISps30Driver.h
│   │   │   └── IIcm20948Driver.h
│   │   ├── platform/                     Arduino/Adafruit concrete implementations
│   │   │   ├── ArduinoClock.h
│   │   │   ├── ArduinoAnalogReader.h
│   │   │   ├── RadioHeadTdmaDriver.h     RHReliableDatagram implementation of ITdmaRadioDriver
│   │   │   └── AdafruitSht31Driver.h
│   │   └── power/
│   │       ├── DutyCycleController.h     Sensor wake/sample/sleep state machine
│   │       └── BatteryMonitor.h
│   ├── src/
│   │   ├── main.cpp                      Constructs all objects, wires them together
│   │   ├── app/SmartFiresNodeApp.cpp
│   │   ├── radio/
│   │   │   ├── PacketHandler.cpp
│   │   │   ├── TdmaRadioService.cpp
│   │   │   ├── TdmaClock.cpp
│   │   │   └── TdmaTxQueue.cpp
│   │   ├── sensors/                      Sensor implementations
│   │   ├── platform/                     Arduino driver implementations
│   │   └── power/
│   └── test/
│       ├── fakes/                        FakeClock, FakeRadio, FakeSensor, etc.
│       └── test_*/                       Unity test suites (run on native)
├── edge/                          Jetson Python code
│   ├── receiver.py                UART frame receiver → CSV writer + TIME_SYNC sender
│   ├── packet.py                  Python mirror of BinaryPacket.h (needs update for bundles)
│   └── requirements.txt           pyserial>=3.5
├── documentation/
│   ├── BINARY_PACKET_PIPELINE.md  Current pipeline design + remaining work
│   ├── FLASHING.md
│   ├── SOFTWARE_DESIGN.md
│   └── old/                       Pre-refactor planning docs (for reference only)
└── lora/                          Legacy experimental LoRa sketches (ignore)
```

---

## Build Environments

| Environment | Board | Key flags | Purpose |
|---|---|---|---|
| `feather_m0_lora_node` | Feather M0 | `NODE_ID=1` `NUM_SLOTS=2` | Sensor node firmware |
| `feather_m0_lora` | Feather M0 | — | Base station (not yet ported) |
| `native` | Desktop | `UNIT_TEST` | Unity unit tests — no hardware required |

### Adding a node

1. Duplicate `feather_m0_lora_node` in `platformio.ini`, set `NODE_ID=N`.
2. **Update `NUM_SLOTS` to the new total node count in ALL node environments.**
3. Reflash every node Feather — they all need the same `NUM_SLOTS` for TDMA to work.

`NUM_SLOTS` is the only flag that must match across all node Feathers. `NODE_ID` is unique per device.

### Common build commands (run from `platformio/`)

```bash
pio run -e feather_m0_lora_node --target upload   # flash sensor node
pio run -e feather_m0_lora      --target upload   # flash base station
pio device monitor -e feather_m0_lora_node        # serial monitor node
pio test -e native                                 # run unit tests on desktop
```

---

## Firmware Architecture

```
main.cpp
  constructs: ArduinoClock, sensors, DutyCycleController,
              PacketHandler, TdmaConfig/Clock/Queue/RadioService,
              RadioHeadTdmaDriver, SmartFiresNodeApp
  setup() → app.begin()
  loop()  → app.update()

SmartFiresNodeApp::update()
  _radio.update()          ← TDMA tick, TIME_SYNC receive
  _duty.update()           ← sensor wake/sample/sleep state machine
  if duty.telemetryReady():
    buildSnapshot()        ← calls sensor.fillSnapshot() on each ISensor*
    packetHandler.push(snapshot)
    if gpsPacketReady()  → enqueueTelemetry(GPS payload, 12 bytes)
    if bundleReady()     → enqueueTelemetry(BUNDLE payload, ≤141 bytes)

PacketHandler
  push(SensorSnapshot):
    tryEncodeGps()         ← emits PKT_GPS once per session on first GPS fix
    quantize() → FullStatePayload (fixed-point integers)
    accumulate reference + up to 7 DeltaPayloads
    on 8th sample: encodeBundlePayload() → bundleReady = true

TdmaRadioService::update()
  checkIncomingTimeSync()  ← polls driver for TIME_SYNC broadcasts
  drainTxQueue()           ← sends one payload per TDMA slot via RadioHeadTdmaDriver
```

### Key data flow

```
ISensor::fillSnapshot(SensorSnapshot&)    float units (tempC, windMps, …)
        ↓
PacketHandler::quantize()                 fixed-point integers (FullStatePayload)
        ↓
BinaryPacket::encodeBundlePayload()       raw LoRa bytes (≤141 bytes)
        ↓
TdmaTxQueue::enqueue()                    4-slot ring buffer, drop-oldest
        ↓
RadioHeadTdmaDriver::sendToWait()         TDMA-gated LoRa TX
```

---

## Wire Protocol

### LoRa payloads — node → base (variable)

```
BUNDLE:  [PktHeader: 4][FullStatePayload: 24][n_deltas: 1][DeltaPayload×n: n×16]  ≤ 141 bytes
GPS:     [PktHeader: 4][GpsPayload: 8]                                               = 12 bytes
```

RadioHead `RHReliableDatagram` handles LoRa framing, ACK, and retry. Node calls
`sendtoWait()` (1 retry, 100 ms timeout); base calls `recvfromAck()` which auto-ACKs.

### LoRa TIME_SYNC broadcast — base → all nodes (12 bytes)

```
[PktHeader: 4][TimeSyncPayload: 8]
```

Sent to `RH_BROADCAST_ADDRESS (0xFF)` — fire-and-forget. Nodes receive it between TX
slots and call `TdmaClock::applySync(sessionMs)`.

### UART frame — base → Jetson (variable length)

```
[0xAA][0x55][len: u8][rssi: i8][LoRa payload][crc8]
  GPS:    len=13  → total frame 17 bytes
  BUNDLE: len≤142 → total frame ≤146 bytes
```

### UART TIME_SYNC frame — Jetson → base (16 bytes)

```
[0xAA][0x55][len=12][PktHeader(PKT_TIME_SYNC): 4][TimeSyncPayload: 8][crc8]
```

### PktHeader (4 bytes, packed)

| Field | Type | Notes |
|---|---|---|
| `magic` | `uint8_t` | `0xA5` |
| `pkt_type` | `uint8_t` | `0x01` FULL_STATE · `0x02` HEARTBEAT · `0x03` TIME_SYNC · `0x04` BUNDLE · `0x05` GPS |
| `node_id` | `uint8_t` | compile-time `NODE_ID`; `0` for TIME_SYNC frames |
| `seq` | `uint8_t` | rolling 0–255 |

### FullStatePayload (24 bytes, packed, little-endian)

| Field | Type | Encoding |
|---|---|---|
| `session_time` | `uint32_t` | ms since Jetson session start (synced via TIME_SYNC) |
| `uptime_ms` | `uint32_t` | local millis() — kept until startup TIME_SYNC handshake is enforced |
| `sensor_flags` | `uint16_t` | WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10 |
| `wind_cms` | `uint16_t` | cm/s = windMps × 100 |
| `temp_cdegc` | `int16_t` | centi-°C = tempC × 100 |
| `humidity_cpct` | `uint16_t` | centi-% = humidityPct × 100 |
| `pm1_0_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm2_5_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm4_0_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm10_ug10` | `uint16_t` | µg/m³ × 10 |

### GpsPayload (8 bytes) — sent once per session via PKT_GPS

| Field | Type | Encoding |
|---|---|---|
| `lat_e7` | `int32_t` | degrees × 1e7 |
| `lon_e7` | `int32_t` | degrees × 1e7 |

### DeltaPayload (16 bytes, packed, little-endian)

`wind_cms` is absolute; all other fields are signed deltas from the bundle reference.

| Field | Type | Encoding |
|---|---|---|
| `dt_ms` | `uint16_t` | ms since reference `session_time` |
| `wind_cms` | `uint16_t` | absolute cm/s |
| `temp_delta_cdegc` | `int16_t` | centi-°C delta |
| `humidity_delta_cpct` | `int16_t` | centi-% delta |
| `pm1_0_delta` | `int16_t` | µg/m³ × 10 delta |
| `pm2_5_delta` | `int16_t` | µg/m³ × 10 delta |
| `pm4_0_delta` | `int16_t` | µg/m³ × 10 delta |
| `pm10_delta` | `int16_t` | µg/m³ × 10 delta |

### TimeSyncPayload (8 bytes, packed, little-endian)

| Field | Type | Notes |
|---|---|---|
| `session_id` | `uint32_t` | Random value set at receiver.py startup. Change triggers GPS re-send and clock reset. |
| `session_time_ms` | `uint32_t` | ms since receiver.py started. Wraps at ~49 days. |

CRC: CRC-8/MAXIM (polynomial 0x31), covers the `len` byte + all data bytes.

---

## TDMA — Time Division Multiple Access

All nodes share the 915 MHz LoRa channel. TDMA divides time into repeating frames,
each split into `NUM_SLOTS` equal slots. Each node transmits only in its assigned slot.

```
|<-------------- NUM_SLOTS × slotWidthMs = frame period ─────────────>|
|  slot 0 (node 1)  |  slot 1 (node 2)  |  ...
|--guard--|--TX win--|--guard--|--TX win--|
    20 ms     860 ms     20 ms     860 ms
```

**Slot = `(NODE_ID - 1) % NUM_SLOTS`** — baked in at compile time via `TdmaConfig`.

| Parameter | Value | Notes |
|---|---|---|
| `slotWidthMs` | 900 ms | Fits worst-case 2×(236 ms TX + 100 ms timeout) + 2×20 ms guard |
| `guardMs` | 20 ms | Covers ≤1.5 ms crystal drift per 30 s sync interval at 50 ppm |
| `maxRetries` | 1 | One re-attempt; ACK typically arrives in ~35 ms |
| `ackTimeoutMs` | 100 ms | Per-attempt timeout |
| `syncStaleMs` | 300 000 ms | After 5 min without TIME_SYNC, fall back to immediate TX |
| `NUM_SLOTS` (build flag) | 2 (default) | Must match across all node Feathers |

TDMA state is managed by `TdmaClock`. The session clock is:

```
sessionNow = syncSessionMs + (millis() - syncLocalMs)
```

Updated on every `TdmaClock::applySync()` call (triggered by incoming TIME_SYNC).

### TX queue and packet freshness

`TdmaTxQueue` holds 4 slots. When full, the oldest entry is evicted — the queue always
holds the freshest data. `TdmaRadioService::drainTxQueue()` sends one payload per slot.

---

## TIME_SYNC flow

```
receiver.py (Jetson)
  │  Sends 16-byte TIME_SYNC UART frame at startup and every 30 s
  ▼
Base Feather  ← NOT YET PORTED
  │  Decodes TimeSyncPayload from UART frame
  │  Broadcasts 12-byte LoRa packet to RH_BROADCAST_ADDRESS
  ▼
Node Feather — TdmaRadioService::checkIncomingTimeSync()
  │  RadioHeadTdmaDriver::receive() — decodes BinaryPacket::TimeSyncPayload
  │  NOTE: driver currently uses placeholder text "TS,<ms>" for native tests;
  │        real binary decode via BinaryPacket::decodeTimeSync() is pending
  │  TdmaClock::applySync(sessionMs) — updates session clock
  ▼
PacketHandler::push(SensorSnapshot)
  snap.sessionTimeMs = TdmaClock::sessionNowMs()   ← attached to every packet
```

---

## Implementation Status

| Item | Status | Notes |
|---|---|---|
| Class-based firmware architecture | **Done** | SmartFiresNodeApp, PacketHandler, TdmaRadioService, TdmaClock, TdmaTxQueue |
| Binary bundle protocol (PKT_BUNDLE) | **Done** | BinaryPacket.h, PacketHandler, encodeBundlePayload() |
| Delta frames | **Done** | Reference + 7 DeltaPayloads per bundle |
| GPS one-shot packet (PKT_GPS) | **Done** | PacketHandler::gpsPacketReady() / takeGpsPacket() / resetGpsSession() |
| SHT31 sensor wired end-to-end | **Done** | fillSnapshot() implemented |
| TDMA clock + slot gating | **Done** | TdmaClock, TdmaRadioService |
| TIME_SYNC binary decode in driver | **Pending** | RadioHeadTdmaDriver uses text placeholder; needs BinaryPacket::decodeTimeSync() |
| Base station port | **Pending** | feather_m0_lora env exists but firmware not ported to new class structure |
| Remaining sensors | **Pending** | Wind, GPS, SPS30, IMU — implement fillSnapshot() as each is wired in |
| Battery + GPS session packet | **Pending** | PKT_SESSION_INFO combining GPS + battery_mv/pct — see BINARY_PACKET_PIPELINE.md |
| edge/packet.py bundle decode | **Pending** | Needs update for PKT_BUNDLE delta expansion and 24-byte FullStatePayload |
| uptime_ms deprecation | **Pending** | Remove once startup TIME_SYNC is guaranteed; saves 4 bytes/bundle |

Full details and design notes in `documentation/BINARY_PACKET_PIPELINE.md`.

---

## Edge Receiver (Jetson)

```bash
pip install -r edge/requirements.txt
python3 edge/receiver.py --output telemetry.csv   # default port /dev/ttyTHS1
python3 edge/receiver.py --sync-interval 10       # faster sync for testing
```

`edge/packet.py` is a Python mirror of `BinaryPacket.h` — currently out of date with
the bundle format and needs to be updated before end-to-end testing.

Jetson UART setup (one-time):
1. `sudo /opt/nvidia/jetson-io/jetson-io.py` — enable the UART pin group
2. `sudo systemctl disable nvgetty && sudo udevadm trigger` — free the port
3. Typical device path: `/dev/ttyTHS1`

---

## Key Design Decisions

- **Single-board node:** Feather M0 owns both sensing and LoRa. No separate ESP32. Simplifies firmware, eliminates UART bridge between boards.
- **Binary not text:** text CSV was ~90 bytes/packet; binary bundle ≤141 bytes carries 8 samples. Effective per-sample cost ~18 bytes vs ~90 bytes.
- **Fixed-point integers on the wire:** no floats — deterministic size, no locale/printf issues on MCUs. `SensorSnapshot` uses floats internally; `PacketHandler::quantize()` converts.
- **SensorSnapshot as internal currency:** sensors write float readings into `SensorSnapshot` via `ISensor::fillSnapshot()`. `PacketHandler` owns all quantization and encoding — sensors never touch wire format.
- **GPS one-shot per session:** `PKT_GPS` is sent once on first valid fix. Suppressed until `PacketHandler::resetGpsSession()` is called on new `session_id`. Saves 8 bytes per bundle.
- **TDMA slot assignment is compile-time:** `slot = (NODE_ID - 1) % NUM_SLOTS`. No runtime negotiation. Adding a node = change `NUM_SLOTS` in platformio.ini, reflash all Feathers.
- **`NUM_SLOTS` must match across all node Feathers.** Mismatch causes slot collisions. Only node Feathers enforce TDMA — base station is unaware.
- **Drop-oldest queue:** `TdmaTxQueue` always holds the freshest data. No blocking between sensing and TX.
- **Stale-sync fallback:** if no TIME_SYNC for 5 min, `TdmaClock::myTurn()` returns true unconditionally — node transmits immediately rather than going silent.
- **TIME_SYNC driven by Jetson NTP, not GPS.** GPS PPS sync deferred; current ≤1.5 ms relative drift between synced nodes is within the 20 ms guard band.
