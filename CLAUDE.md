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
    |   Node → Base: AWAKEN payload (4 bytes, on boot until TIME_SYNC received)
    |                BUNDLE payload (≤193 bytes, 1 retry, 100 ms timeout, TDMA-gated)
    |                STATUS payload (16 bytes, GPS + battery, every 15 min)
    |   Base → Node: TIME_SYNC broadcast (12 bytes, fire-and-forget, RH_BROADCAST_ADDRESS)
    v
[Adafruit Feather M0 RFM95 — base station]  ← NOT YET PORTED to class architecture
    Receives telemetry, auto-ACKs, relays to Jetson over UART.
    Reads TIME_SYNC commands from Jetson UART, broadcasts them over LoRa every 10 min.
    |
    | UART 115200 baud (Serial1, binary frames — bidirectional)
    |   Feather → Jetson: base UART frames with RSSI (variable length, ≤198 bytes)
    |   Jetson → Feather: TIME_SYNC command frames (16 bytes, every 10 min)
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
│   │   │   ├── PacketHandler.h           Bundle + STATUS packet assembly ← main protocol entry
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
│   ├── packet.py                  Python mirror of BinaryPacket.h
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
  setup() → app.begin()   ← broadcasts PKT_AWAKEN immediately
  loop()  → app.update()

SmartFiresNodeApp::update()
  _radio.update()          ← TDMA tick, TIME_SYNC receive
  if !tdmaClock.hasSync():
    re-send PKT_AWAKEN every 5 s, return   ← sensors idle until synced
  _duty.update()           ← sensor wake/sample/sleep state machine
  if duty.telemetryReady():
    buildSnapshot()        ← calls sensor.fillSnapshot() + battery reading;
                              timestamps via TdmaClock::sessionNowMs()
    packetHandler.push(snapshot)
    if statusPacketReady() → enqueueTelemetry(STATUS payload, 16 bytes)
    if bundleReady()       → enqueueTelemetry(BUNDLE payload, ≤193 bytes)

PacketHandler
  push(SensorSnapshot):
    tryEncodeStatus()      ← emits PKT_STATUS on first push, then every 15 min
    quantize() → FullStatePayload (fixed-point integers)
    accumulate reference + up to 14 DeltaPayloads
    on 15th sample: encodeBundlePayload() → bundleReady = true

TdmaRadioService::update()
  checkIncomingTimeSync()  ← polls driver, decodes binary BinaryPacket::TimeSyncPayload
  drainTxQueue()           ← sends multiple payloads per TDMA slot while budget allows
                              and app-layer reliability retransmits pending misses
```

### Key data flow

```
ISensor::fillSnapshot(SensorSnapshot&)    float units (tempC, windMps, …)
        ↓
PacketHandler::quantize()                 fixed-point integers (FullStatePayload)
        ↓
BinaryPacket::encodeBundlePayload()       raw LoRa bytes (≤193 bytes)
        ↓
TdmaTxQueue::enqueue()                    4-slot ring buffer, drop-oldest
        ↓
RadioHeadTdmaDriver::sendToWait()         TDMA-gated LoRa TX
RadioHeadTdmaDriver::send()               non-blocking LoRa TX for fresh telemetry
```

---

## Wire Protocol

### LoRa payloads — node → base

```
AWAKEN:  [PktHeader: 4]                                                              =  4 bytes
BUNDLE:  [PktHeader: 4][FullStatePayload: 20][n_deltas: 1][DeltaPayload×n: n×12]  ≤ 193 bytes
STATUS:  [PktHeader: 4][StatusPayload: 12]                                           = 16 bytes
```

RadioHead `RHReliableDatagram` handles LoRa framing and addressing.
Node fresh telemetry uses non-blocking `sendto()` with app-layer reliability; the
base still receives via `recvfromAck()` and auto-ACKs at the LoRa link layer.

### LoRa TIME_SYNC broadcast — base → all nodes (12 bytes)

```
[PktHeader: 4][TimeSyncPayload: 8]
```

Sent to `RH_BROADCAST_ADDRESS (0xFF)` — fire-and-forget. Nodes receive it between TX
slots and call `TdmaClock::applySync(sessionMs)`.

### UART frame — base → Jetson (variable length)

```
[0xAA][0x55][len: u8][rssi: i8][LoRa payload][crc8]
  AWAKEN: len=5   → total frame  9 bytes
  STATUS: len=17  → total frame 21 bytes
  BUNDLE: len≤194 → total frame ≤198 bytes
```

### UART TIME_SYNC frame — Jetson → base (16 bytes)

```
[0xAA][0x55][len=12][PktHeader(PKT_TIME_SYNC): 4][TimeSyncPayload: 8][crc8]
```

### PktHeader (4 bytes, packed)

| Field | Type | Notes |
|---|---|---|
| `magic` | `uint8_t` | `0xA5` |
| `pkt_type` | `uint8_t` | `0x01` FULL_STATE · `0x02` HEARTBEAT · `0x03` TIME_SYNC · `0x04` BUNDLE · `0x05` STATUS · `0x06` AWAKEN |
| `node_id` | `uint8_t` | compile-time `NODE_ID`; `0` for TIME_SYNC frames |
| `seq` | `uint8_t` | rolling 0–255 |

### FullStatePayload (20 bytes, packed, little-endian)

| Field | Type | Encoding |
|---|---|---|
| `session_time` | `uint32_t` | ms since Jetson session start (from TdmaClock::sessionNowMs()) |
| `sensor_flags` | `uint16_t` | WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10 |
| `wind_cms` | `uint16_t` | cm/s = windMps × 100 |
| `temp_cdegc` | `int16_t` | centi-°C = tempC × 100 |
| `humidity_cpct` | `uint16_t` | centi-% = humidityPct × 100 |
| `pm1_0_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm2_5_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm4_0_ug10` | `uint16_t` | µg/m³ × 10 |
| `pm10_ug10` | `uint16_t` | µg/m³ × 10 |

### StatusPayload (12 bytes) — sent every 15 min via PKT_STATUS

| Field | Type | Encoding |
|---|---|---|
| `lat_e7` | `int32_t` | degrees × 1e7 (valid if `flags & STATUS_GPS_VALID`) |
| `lon_e7` | `int32_t` | degrees × 1e7 (valid if `flags & STATUS_GPS_VALID`) |
| `battery_mv` | `uint16_t` | millivolts (valid if `flags & STATUS_BATT_VALID`) |
| `battery_pct` | `uint8_t` | 0–100 (valid if `flags & STATUS_BATT_VALID`) |
| `flags` | `uint8_t` | STATUS_GPS_VALID=0x01 · STATUS_BATT_VALID=0x02 |

### DeltaPayload (12 bytes, packed, little-endian)

`wind_cms` is absolute; remaining fields are compact deltas from the bundle reference.

| Field | Type | Encoding |
|---|---|---|
| `dt_ticks_250ms` | `uint8_t` | ticks since reference; `dt_ms = ticks × 250` |
| `wind_cms` | `uint16_t` | absolute cm/s |
| `temp_delta_deci_c` | `int8_t` | 0.1 °C delta |
| `humidity_delta_0p2pct` | `int8_t` | 0.2 %RH delta |
| `pm1_0_delta_ug` | `int8_t` | 1.0 µg/m³ delta |
| `pm2_5_delta_ug10` | `int16_t` | 0.1 µg/m³ delta |
| `pm4_0_delta_ug` | `int8_t` | 1.0 µg/m³ delta |
| `pm10_delta_ug10` | `int16_t` | 0.1 µg/m³ delta |
| `flags` | `uint8_t` | clamp/overflow bitmask |

### TimeSyncPayload (8 bytes, packed, little-endian)

| Field | Type | Notes |
|---|---|---|
| `session_id` | `uint32_t` | Random value set at receiver.py startup. Change triggers STATUS re-send and clock reset. |
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
| `slotWidthMs` | 900 ms | Fits worst-case 2×(313 ms TX + 100 ms timeout) + 2×20 ms guard |
| `guardMs` | 20 ms | Covers crystal drift between 10-min sync intervals at 50 ppm |
| `maxRetries` | 1 | One re-attempt; ACK typically arrives in ~35 ms |
| `ackTimeoutMs` | 100 ms | Per-attempt timeout |
| `syncStaleMs` | 1 320 000 ms | After 22 min without TIME_SYNC, fall back to immediate TX |
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

## Boot handshake + TIME_SYNC flow

```
Node Feather — SmartFiresNodeApp::begin()
  │  Broadcasts PKT_AWAKEN (4 bytes) to base station address
  │  Re-broadcasts every 5 s while waiting
  │  Sensors and duty cycle are HELD OFF until sync is received
  ▼
Base Feather  ← NOT YET PORTED
  │  Relays PKT_AWAKEN to Jetson over UART
  │  Receives TIME_SYNC UART frame from Jetson
  │  Broadcasts 12-byte LoRa TIME_SYNC to RH_BROADCAST_ADDRESS (every 10 min)
  ▼
Node Feather — TdmaRadioService::checkIncomingTimeSync()
  │  RadioHeadTdmaDriver::receive() — decodes BinaryPacket::TimeSyncPayload (binary)
  │  TdmaClock::applySync(sessionMs) — sets hasSync() = true, updates session clock
  ▼
SmartFiresNodeApp::update() — sensing begins
  snap.sessionTimeMs = TdmaClock::sessionNowMs()   ← attached to every packet
```

---

## Implementation Status

| Item | Status | Notes |
|---|---|---|
| Class-based firmware architecture | **Done** | SmartFiresNodeApp, PacketHandler, TdmaRadioService, TdmaClock, TdmaTxQueue |
| Binary bundle protocol (PKT_BUNDLE) | **Done** | BinaryPacket.h, PacketHandler, encodeBundlePayload() |
| Delta frames | **Done** | Reference + 14 DeltaPayloads per bundle (12-byte compact delta) |
| PKT_STATUS periodic packet | **Done** | GPS + battery every 15 min; PacketHandler::statusPacketReady() / takeStatusPacket() / resetStatusTimer() |
| AWAKEN boot handshake | **Done** | Node broadcasts PKT_AWAKEN every 5 s until TIME_SYNC received; sensors withheld until synced |
| uptime_ms removed | **Done** | FullStatePayload is now 20 bytes; all timestamps from TdmaClock::sessionNowMs() |
| SHT31 sensor wired end-to-end | **Done** | fillSnapshot() implemented |
| TDMA clock + slot gating | **Done** | TdmaClock, TdmaRadioService |
| TIME_SYNC binary decode | **Done** | TdmaRadioService uses BinaryPacket::decodeTimeSync() |
| Base station port | **Pending** | feather_m0_lora env exists but firmware not ported to new class structure |
| Remaining sensors | **Pending** | Wind, GPS, SPS30, IMU — implement fillSnapshot() as each is wired in |
| edge/packet.py bundle decode | **Done** | Updated for 20-byte FullStatePayload + compact 12-byte deltas |

Full details and design notes in `documentation/BINARY_PACKET_PIPELINE.md`.
Sizing and scaling math tables are in `documentation/BANDWIDTH_SCALING.md`.

---

## Edge Receiver (Jetson)

```bash
pip install -r edge/requirements.txt
python3 edge/receiver.py --output telemetry.csv   # default port /dev/ttyTHS1
python3 edge/receiver.py --sync-interval 600      # 10-min sync interval
```

`edge/packet.py` mirrors `BinaryPacket.h` for STATUS/FULL_STATE/BUNDLE parsing and
bundle delta expansion.

Jetson UART setup (one-time):
1. `sudo /opt/nvidia/jetson-io/jetson-io.py` — enable the UART pin group
2. `sudo systemctl disable nvgetty && sudo udevadm trigger` — free the port
3. Typical device path: `/dev/ttyTHS1`

---

## Key Design Decisions

- **Single-board node:** Feather M0 owns both sensing and LoRa. No separate ESP32. Simplifies firmware, eliminates UART bridge between boards.
- **Binary not text:** text CSV was ~90 bytes/packet; binary bundle ≤193 bytes carries 15 samples. Effective per-sample cost ~13 bytes vs ~90 bytes.
- **Fixed-point integers on the wire:** no floats — deterministic size, no locale/printf issues on MCUs. `SensorSnapshot` uses floats internally; `PacketHandler::quantize()` converts.
- **SensorSnapshot as internal currency:** sensors write float readings into `SensorSnapshot` via `ISensor::fillSnapshot()`. Battery data is also written into the snapshot by `SmartFiresNodeApp`. `PacketHandler` owns all quantization and encoding — sensors never touch wire format.
- **AWAKEN boot handshake:** node withholds sensing until it receives a TIME_SYNC from the base. This ensures `session_time` in every bundle is valid from the first sample. Node re-broadcasts AWAKEN every 5 s until synced.
- **PKT_STATUS every 15 min:** replaces the old one-shot PKT_GPS. Carries GPS position + battery level with validity flags so the receiver always knows what's populated. First STATUS goes out on the first sensing cycle after sync.
- **TDMA slot assignment is compile-time:** `slot = (NODE_ID - 1) % NUM_SLOTS`. No runtime negotiation. Adding a node = change `NUM_SLOTS` in platformio.ini, reflash all Feathers.
- **`NUM_SLOTS` must match across all node Feathers.** Mismatch causes slot collisions. Only node Feathers enforce TDMA — base station is unaware.
- **Drop-oldest queue:** `TdmaTxQueue` always holds the freshest data. No blocking between sensing and TX.
- **Stale-sync fallback:** if no TIME_SYNC for 22 min (2× the 10-min broadcast interval), `TdmaClock::myTurn()` returns true unconditionally — node transmits immediately rather than going silent.
- **TIME_SYNC driven by Jetson NTP, not GPS.** GPS PPS sync deferred; current crystal drift between synced nodes is within the 20 ms guard band.
