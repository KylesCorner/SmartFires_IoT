# SmartFires IoT — Claude Context

## Agent Execution Guardrails

- Do not run `pio run`, `pio test`, `pio device monitor`, flashing commands, or other hardware-facing PlatformIO commands unless the user explicitly asks for execution in the current session.
- Default behavior for embedded validation in this repo is: explain what should be run and give the exact command for the user to execute.
- Reason: hardware availability, connected board selection, active serial monitor state, and local PlatformIO context are session-specific and may not be safe for an agent to assume.

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
    |   Node → Base: AWAKEN payload (9 bytes, on boot until TIME_SYNC received)
    |                BUNDLE payload (≤194 bytes, app-layer ACK-paced retry, TDMA-gated)
    |                STATUS payload (25 bytes, GPS + battery + DMP heading + link stats, every 15 min)
    |                CMD_ACK (11 bytes, acknowledges CALIBRATE/RESET commands)
    |   Base → Node: TIME_SYNC broadcast (13 bytes, fire-and-forget, RH_BROADCAST_ADDRESS)
    |                ACK_SUMMARY (9 bytes, base-generated app-layer reliability bitmap)
    |                CMD_CALIBRATE (7 bytes, forwarded from Jetson)
    |                CMD_RESET (7 bytes, forwarded from Jetson)
    v
[Adafruit Feather M0 RFM95 — base station]  ← SmartFiresBaseApp, fully ported
    Receives telemetry, auto-ACKs, relays to Jetson over USB.
    Assigns node_id from uid_hash on first AWAKEN (findOrCreateNodeAssignment).
    Reads TIME_SYNC + command frames from the Jetson USB link, routes/broadcasts over LoRa.
    |
    | USB CDC, 115200 baud (Serial — native USB, binary frames — bidirectional)
    |   Feather → Jetson: base UART frames with RSSI (variable length, ≤198 bytes)
    |   Jetson → Feather: TIME_SYNC (16 bytes), CMD_CALIBRATE/RESET (11 bytes)
    |   (ACK_SUMMARY is generated locally by the base itself, not sent by the Jetson)
    v
[Jetson Orin Nano]
  edge/edge-receiver/src/smartfires_edge/ingest_service.py → rotated telemetry CSV
    Sends periodic TIME_SYNC frames to keep all nodes on a common session clock.
    Reads DMP-computed heading from STATUS packet (on-chip fusion, no Jetson calibration pipeline).
    `smartfires-edge web`: FastAPI/uvicorn dashboard (live map, sniffer, debug log) + REST/WS API.
    No CLI exists yet to actually send CMD_CALIBRATE/CMD_RESET — only passive decode (sniffer_service.py)
    and a stub `/api/command` endpoint that echoes back without sending.
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
│   │   ├── config/                       ← tunable constants (single source of truth)
│   │   │   ├── NetworkConfig.h           TDMA geometry, radio link, reliability, TX budgets
│   │   │   ├── SensingConfig.h           Duty-cycle presets + per-sensor timing/calibration
│   │   │   ├── PowerConfig.h             Battery monitor constants
│   │   │   └── BaseConfig.h              Base-station bridge constants (shares NetworkConfig)
│   │   ├── app/
│   │   │   ├── SmartFiresNodeApp.h       Top-level node application class
│   │   │   └── SmartFiresBaseApp.h       Top-level base-station application class
│   │   ├── calibration/
│   │   │   └── CalibrationDebug.h        CMD_CALIBRATE/RESET/ACK name/status helpers + dispatch
│   │   ├── logging/
│   │   │   ├── DebugLogger.h             @SFDBG structured log macros
│   │   │   └── FramedDebugLogSink.h      Flushes log lines as PKT_DEBUG_LOG frames over USB
│   │   ├── interfaces/
│   │   │   ├── IClock.h                  millis() abstraction
│   │   │   ├── ISensor.h                 Sensor interface + fillSnapshot()
│   │   │   ├── IRadio.h
│   │   │   ├── IAnalogReader.h
│   │   │   └── Itps.h
│   │   ├── radio/
│   │   │   ├── PacketHandler.h           Bundle + STATUS packet assembly ← main protocol entry
│   │   │   ├── TdmaRadioService.h        TDMA-gated LoRa TX/RX orchestration
│   │   │   ├── TdmaClock.h               Session clock + slot timing
│   │   │   ├── TdmaConfig.h              All TDMA parameters in one struct
│   │   │   ├── TdmaTxQueue.h             8-entry drop-oldest ring buffer
│   │   │   └── ITdmaRadioDriver.h        Hardware abstraction for radio
│   │   ├── telemetry/
│   │   │   ├── BinaryPacket.h            Wire format: structs, CRC, encoders ← main protocol file
│   │   │   └── SensorSnapshot.h          Internal float-unit sensor reading struct
│   │   ├── sensors/
│   │   │   ├── Sht31Sensor.h             (impl in src/sensors/)
│   │   │   ├── WindSensorRevC.h
│   │   │   ├── Pa1010dGpsSensor.h
│   │   │   ├── Sps30Sensor.h
│   │   │   ├── Icm20948Sensor.h
│   │   │   └── ITriggerSensor.h
│   │   ├── drivers/                      Hardware driver interfaces (I2C, SPI)
│   │   │   ├── ISht31Driver.h
│   │   │   ├── IGpsDriver.h
│   │   │   ├── ISps30Driver.h
│   │   │   └── IIcm20948Driver.h
│   │   ├── platform/                     Arduino/Adafruit concrete implementations
│   │   │   ├── ArduinoClock.h
│   │   │   ├── ArduinoAnalogReader.h
│   │   │   ├── BoardIdentify.h           SAMD21 unique-ID → uid_hash
│   │   │   ├── RadioHeadTdmaDriver.h     RHReliableDatagram implementation of ITdmaRadioDriver
│   │   │   ├── AdafruitSht31Driver.h
│   │   │   ├── AdafruitGpsDriver.h
│   │   │   ├── SensirionUartSps30Driver.h
│   │   │   ├── SparkfunIcm20948Driver.h  DMP 9DOF rotation-vector driver
│   │   │   └── TPSDriver.h
│   │   └── power/
│   │       ├── DutyCycleController.h     Sensor wake/sample/sleep state machine
│   │       └── BatteryMonitor.h
│   ├── src/
│   │   ├── main.cpp                      Constructs all objects, wires them together
│   │   ├── main_lora_sniffer.cpp         Passive sniffer entrypoint
│   │   ├── app/SmartFiresNodeApp.cpp, SmartFiresBaseApp.cpp
│   │   ├── calibration/CalibrationDebug.cpp
│   │   ├── radio/
│   │   │   ├── PacketHandler.cpp
│   │   │   ├── TdmaRadioService.cpp
│   │   │   ├── TdmaClock.cpp
│   │   │   └── TdmaTxQueue.cpp
│   │   ├── sensors/                      Sensor implementations (all fillSnapshot() done)
│   │   ├── platform/                     Arduino driver implementations
│   │   └── power/
│   └── test/
│       ├── support/fakes/                FakeClock, FakeRadio, FakeSensor, etc.
│       └── test_*/                       Unity test suites (run on native)
├── edge/                          Jetson Python code
│   ├── anemometer_read.py         Standalone ES-W302 reader (uses smartfires_edge module)
│   └── edge-receiver/
│       ├── pyproject.toml         Package + dependencies (pyserial, minimalmodbus, numpy, geomag, fastapi, uvicorn)
│       ├── README.md
│       └── src/smartfires_edge/
│           ├── main.py             CLI entrypoint (`smartfires-edge`): receive / web / visualize / summary
│           ├── config.py           All tunable defaults (port/baud/intervals/timeouts) + argparse helpers
│           ├── ingest_service.py   UART ingest + TIME_SYNC + ACK summary + CSV logging
│           ├── packet.py           Python mirror of BinaryPacket.h (FULL_STATE/STATUS/BUNDLE/CMD_*)
│           ├── uart_receiver.py    UART frame parser state machine
│           ├── anemometer.py       ES-W302 polling module for integrated local wind logging
│           ├── session.py          Heading/declination correction, node state (SessionManager)
│           ├── session_meta.py     Writes session.json manifest (session id, port/baud, node registry)
│           ├── base_station_store.py  Persists base-station lat/lon (~/.smartfires/base_station.json)
│           ├── packet_loss.py      Seq-gap-based loss/duplicate tracking (8-bit wraparound aware)
│           ├── live_state.py       Thread-safe in-memory store feeding the web dashboard
│           ├── state_store.py      Generic atomic JSON read/write helper
│           ├── debug_log.py        Parses @SFDBG structured lines from PKT_DEBUG_LOG frames
│           ├── models.py           DecodedPacket dataclass for CSV row shaping
│           ├── sniffer_service.py  Decodes passive-sniffer NDJSON; TDMA slot/jitter stats
│           ├── visualize_service.py  `visualize` subcommand: live terminal telemetry tables
│           ├── web_service.py      `web` subcommand: orchestrates ingest + sniffer threads + uvicorn
│           ├── tile_cache.py       Disk-backed XYZ map tile cache (browser-uploaded, per ToS)
│           └── web/                FastAPI app + static dashboard (live map, sniffer, debug log, map history)
├── util/                          Standalone Jetson-side scripts (not part of smartfires_edge package)
│   ├── lora_sniffer_dashboard.py
│   ├── plot_drone_data.py
│   ├── rsync_from_jetson.sh
│   ├── scope_current_log.py
│   ├── smartfires_plots.html
│   └── udev/99-smartfires.rules  Stable /dev/smartfires-base symlink for base + sniffer USB-CDC
├── documentation/
│   ├── README.md                  ← doc index — start here
│   ├── SOFTWARE_DESIGN.md         Master system architecture
│   ├── SOFTWARE_DESIGN_DIAGRAM.md
│   ├── Current_Architecture/      ← subsystem deep-dives (current state)
│   │   ├── TUNABLE_PARAMETERS.md  Every tunable constant — TDMA, sensors, power, Jetson
│   │   ├── TDMA_PROTOCOL.md       Slot timing, session clock, boot handshake, TX budget
│   │   ├── PACKET_RELIABILITY.md  StrictLinkAck vs AppLayerAckSummary, pending window, ACK_SUMMARY
│   │   ├── DUTY_CYCLING.md        DutyCycleController phases, config, trigger sensor
│   │   ├── UART_JETSON_BRIDGE.md  Frame format, FrameReceiver, ingest loop, SessionManager
│   │   └── BANDWIDTH_SCALING.md   Airtime math, node-count scaling table
│   ├── User_Reference/            ← practical how-to guides
│   │   ├── FLASHING.md
│   │   ├── DEBUG_FILTER.md
│   │   ├── JETSON_CHEATSHEET.md
│   │   └── NETWORK_TEST.md
│   ├── Pending_Plans/             ← not-yet-built designs (not current state)
│   │   ├── JETSON_SENSOR_EXPANSION.md
│   │   └── RESET_SYSTEM.md
│   └── Completed_Plans/           ← historical design docs (code is now authoritative)
```

---

## Build Environments

| Environment | Board | Key flags | Purpose |
|---|---|---|---|
| `feather_m0_lora_node` | Feather M0 | `LORA_NODE=2` `NUM_SLOTS=4` `SMARTFIRES_TDMA_RELIABILITY_MODE=1` `SMARTFIRES_STATUS_INTERVAL_MS=15000` `ICM_20948_USE_DMP` | Real sensor node firmware |
| `feather_m0_lora_node_debug` | Feather M0 | `LORA_NODE=1` `NUM_SLOTS=4` `SMARTFIRES_TDMA_RELIABILITY_MODE=1` `SMARTFIRES_STATUS_INTERVAL_MS=1000` `ICM_20948_USE_DMP` | Default env; debug filter, faster STATUS |
| `feather_m0_lora_base` | Feather M0 | `LORA_BASE=1` | Base station firmware |
| `feather_m0_sensor_probe` | Feather M0 | `SENSOR_PROBE=1` `ICM_20948_USE_DMP` | No LoRa/TDMA/app layer; sensor bring-up + power draw measurement |
| `feather_m0_lora_sniffer` | Feather M0 | `SMARTFIRES_LORA_SNIFFER=1` | Passive LoRa packet monitor |
| `native` | Desktop | `UNIT_TEST` | Unity unit tests — no hardware required |

`SMARTFIRES_TDMA_RELIABILITY_MODE=1` selects `AppLayerAckSummary` mode (0 = `StrictLinkAck`) — see
`documentation/Current_Architecture/PACKET_RELIABILITY.md`; this is the mode both real node
environments ship with today.

`NODE_ID` is **not** set for the real node environments — nodes derive their identity at runtime from the SAMD21 128-bit serial number via FNV-1a hash (`uid_hash`). The base station assigns `node_id` from this hash on first `AWAKEN`.

### Adding a node

1. Add a new `[env:feather_m0_lora_node_N]` section in `platformio.ini` based on `feather_m0_lora_node`.
2. **Update `NUM_SLOTS` to (new total node count) + 1 in ALL node environments (node and debug).** Slot 0 is permanently reserved for the base station — `NUM_SLOTS` is never just the node count.
3. Reflash every node Feather — they all need the same `NUM_SLOTS` for TDMA to work.
4. **The base Feather also needs `NUM_SLOTS` to match, but its `[env:feather_m0_lora_base]` section currently doesn't set the flag at all** — it silently falls back to `NetworkConfig.h`'s `#define NUM_SLOTS 4` default via `BaseConfig.h`. This only matches today because node deployments also use 4. If you change node `NUM_SLOTS` away from 4, add `-DNUM_SLOTS=<value>` to the base env explicitly (or wire up a shared `platformio.ini` value so it can't drift) and reflash the base, or the base's slot-0 reservation will silently disagree with the nodes' slot math.

`NUM_SLOTS` is the only flag that must match across all node Feathers (and, per the caveat above, the base).

### Common build commands (run from `platformio/`)

```bash
pio run -e feather_m0_lora_node --target upload   # flash sensor node
pio run -e feather_m0_lora_base --target upload   # flash base station
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
    if statusPacketReady() → enqueueTelemetry(STATUS payload, 25 bytes)
    if bundleReady()       → enqueueTelemetry(BUNDLE payload, ≤194 bytes)

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
BinaryPacket::encodeBundlePayload()       raw LoRa bytes (≤194 bytes)
        ↓
TdmaTxQueue::enqueue()                    8-entry ring buffer, drop-oldest
        ↓
RadioHeadTdmaDriver::sendToWait()         TDMA-gated LoRa TX
RadioHeadTdmaDriver::send()               non-blocking LoRa TX for fresh telemetry
```

---

## Wire Protocol

### LoRa payloads — node → base

```
AWAKEN:  [PktHeader: 4][AwakenPayload: 4][crc8: 1]                                    =  9 bytes
BUNDLE:  [PktHeader: 4][FullStatePayload: 20][n_deltas: 1][DeltaPayload×n: n×12][crc8] ≤ 194 bytes
STATUS:  [PktHeader: 4][StatusPayload: 20][crc8: 1]                                    = 25 bytes
CMD_ACK: [PktHeader: 4][CmdAckPayload: 6][crc8: 1]                                    = 11 bytes
```

RadioHead `RHReliableDatagram` handles LoRa framing and addressing.
Node fresh telemetry uses non-blocking `sendto()` with app-layer reliability; the
base still receives via `recvfromAck()` and auto-ACKs at the LoRa link layer.

### LoRa TIME_SYNC broadcast — base → all nodes (13 bytes)

```
[PktHeader: 4][TimeSyncPayload: 8][crc8: 1]
```

Sent to `RH_BROADCAST_ADDRESS (0xFF)` — fire-and-forget. Nodes receive it between TX
slots and call `TdmaClock::applySync(sessionMs)`.

### UART frame — base → Jetson (variable length)

```
[0xAA][0x55][len: u8][rssi: i8][LoRa payload][crc8]
  AWAKEN: len=10  → total frame 14 bytes
  STATUS: len=26  → total frame 30 bytes
  BUNDLE: len≤195 → total frame ≤199 bytes
```

### UART TIME_SYNC frame — Jetson → base (16 bytes)

```
[0xAA][0x55][len=12][PktHeader(PKT_TIME_SYNC): 4][TimeSyncPayload: 8][crc8]
```

### PktHeader (4 bytes, packed)

| Field | Type | Notes |
|---|---|---|
| `magic` | `uint8_t` | `0xA5` |
| `pkt_type` | `uint8_t` | see packet type table below |
| `node_id` | `uint8_t` | compile-time `NODE_ID`; `0` for broadcast/command frames |
| `seq` | `uint8_t` | rolling 0–255 |

### Packet Types

| Value | Name | Direction | Size (LoRa payload) | Notes |
|---|---|---|---|---|
| `0x01` | PKT_FULL_STATE | Node→Jetson | 25 bytes | Reserved — handled in decode dispatch, never encoded/sent by current firmware |
| `0x02` | PKT_HEARTBEAT | — | — | Reserved, unused |
| `0x03` | PKT_TIME_SYNC | Base→Nodes | 13 bytes | Broadcast, fire-and-forget |
| `0x04` | PKT_BUNDLE | Node→Jetson | ≤194 bytes | 15 samples (ref + 14 deltas) |
| `0x05` | PKT_STATUS | Node→Jetson | 25 bytes | GPS + battery + DMP heading + link stats, every 15 min |
| `0x06` | PKT_AWAKEN | Node→Base | 9 bytes | Boot handshake; contains uid_hash |
| `0x07` | PKT_ACK_SUMMARY | Base→Node | 9 bytes | Base-generated app-layer reliability bitmap (not relayed from Jetson) |
| `0x10` | PKT_CMD_CALIBRATE | Jetson→Node | 7 bytes | Forwarded by base; node just logs + ACKs (DMP self-calibrates regardless) |
| `0x11` | PKT_CMD_RESET | Jetson→Node | 7 bytes | Forwarded by base; node logs + ACKs but does not yet actually reset |
| `0x12` | PKT_CALIBRATION_DATA | — | — | Reserved, unused — no encode/decode functions exist |
| `0x13` | PKT_CMD_ACK | Node→Jetson | 11 bytes | Acknowledge CALIBRATE or RESET; relayed through base |
| `0x14` | PKT_DEBUG_LOG | Base→Jetson | variable | USB/UART only, never sent over LoRa; carries `@SFDBG` text lines (`FramedDebugLogSink`) |

Types `0x10`/`0x11`/`0x13` (CMD_CALIBRATE/CMD_RESET/CMD_ACK) are **implemented end-to-end**:
`SmartFiresNodeApp.cpp` and `CalibrationDebug.cpp` handle them on the node, `SmartFiresBaseApp.cpp`
relays Jetson-originated commands and node ACKs, and `edge-receiver`'s `packet.py`/`config.py`/
`sniffer_service.py` encode/decode them on the Jetson side.

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

### StatusPayload (20 bytes) — sent every 15 min via PKT_STATUS

flags bits: STATUS_GPS_VALID=0x01 · STATUS_BATT_VALID=0x02 · STATUS_IMU_VALID=0x04

| Field | Type | Encoding |
|---|---|---|
| `lat_e7` | `int32_t` | degrees × 1e7 (valid if STATUS_GPS_VALID) |
| `lon_e7` | `int32_t` | degrees × 1e7 (valid if STATUS_GPS_VALID) |
| `battery_mv` | `uint16_t` | millivolts (valid if STATUS_BATT_VALID) |
| `battery_pct` | `uint8_t` | 0–100 (valid if STATUS_BATT_VALID) |
| `flags` | `uint8_t` | GPS_VALID=0x01 · BATT_VALID=0x02 · IMU_VALID=0x04 |
| `heading_deg_x10` | `uint16_t` | heading × 10, 0–3590 (valid if STATUS_IMU_VALID) |
| `heading_accuracy` | `uint16_t` | Q12 raw; divide by 4096 for degrees |
| `retx_total` | `uint16_t` | lifetime LoRa retransmit count, saturated at 65535 |
| `fail_total` | `uint16_t` | lifetime LoRa send-failure count, saturated at 65535 |

`retx_total`/`fail_total` are lifetime totals from node boot (never reset), fed by
`TdmaRadioService::retransmitCount()`/`failedSendCount()` via `PacketHandler::setLinkStats()`.
The Jetson computes per-interval deltas by differencing consecutive STATUS packets — this
lets retry-density be correlated with GPS position with no separate packet type or join.

### DeltaPayload (12 bytes, packed, little-endian)

`wind_cms` is absolute; remaining fields are compact deltas from the bundle reference.

| Field | Type | Encoding |
|---|---|---|
| `dt_ticks_250ms` | `uint8_t` | ticks since **previous sample** (not reference); `dt_ms = ticks × 250` |
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
| `session_id` | `uint32_t` | Random value set at `smartfires-edge receive` startup. Change triggers STATUS re-send and clock reset. |
| `session_time_ms` | `uint32_t` | ms since `smartfires-edge receive` started. Wraps at ~49 days. |

CRC: CRC-8/MAXIM (polynomial 0x31), covers the `len` byte + all data bytes.

---

## TDMA — Time Division Multiple Access

All nodes share the 915 MHz LoRa channel. TDMA divides time into repeating frames,
each split into `NUM_SLOTS` equal slots. **Slot 0 is permanently reserved for the
base station** — node IDs are assigned starting at `kFirstNodeId = 2`
(`config/BaseConfig.h`), so node 1 (→ slot 0) is never given to a real node. Each
real node transmits only in its assigned slot; the base transmits only in slot 0
(`SmartFiresBaseApp::_baseTdmaClock`, constructed with `nodeId=1`) — every
base-originated send (`TIME_SYNC`, `ACK_SUMMARY`, `CMD_CALIBRATE`/`CMD_RESET`) is
deferred until that window is open, never sent immediately.

```
|<-------------- NUM_SLOTS × slotWidthMs = frame period ─────────────────────>|
|  slot 0 (base)  |  slot 1 (node 2)  |  slot 2 (node 3)  |  ...
|--guard--|--TX win--|--guard--|--TX win--|
    20 ms     860 ms     20 ms     860 ms
```

**Slot = `(NODE_ID - 1) % NUM_SLOTS`** — baked in at compile time via `TdmaConfig`.
**With N real nodes, `NUM_SLOTS` must be `N + 1`** (one slot for the base + one per
node) — not `N`.

| Parameter | Value | Notes |
|---|---|---|
| `slotWidthMs` | 900 ms | Fits worst-case bundle TX (340 ms) + link-ACK timeout (250 ms) + 2×20 ms guard |
| `guardMs` | 20 ms | Covers crystal drift between 10-min sync intervals at 50 ppm |
| `kLinkRetries` | 3 | Link-layer (RHReliableDatagram) retries — only active under `StrictLinkAck` mode |
| `kLinkAckTimeoutMs` | 250 ms | Link-layer per-attempt timeout — only active under `StrictLinkAck` mode |
| `syncStaleMs` | 1 320 000 ms | After 22 min without TIME_SYNC, fall back to immediate TX |
| `NUM_SLOTS` (build flag) | 4 (default) | = (node count) + 1; must match across all node Feathers |

All values above live in `config/NetworkConfig.h`. Real node builds run in `AppLayerAckSummary`
mode (`SMARTFIRES_TDMA_RELIABILITY_MODE=1`), where link-layer ACK is disabled
(`enableLinkAck=false`) and reliability instead comes from the base's periodic `ACK_SUMMARY`
bitmap plus app-layer retry gating (`kReliabilityWindowDepth=8`, `kReliabilityMaxAttempts=3`,
`kReliabilityMaxAgeMs=30000`, retry-wait derived from `kExpectedAckIntervalMs`/
`kRetryWaitMultiplierPermille`, clamped to `[kRetryWaitMinMs, kRetryWaitMaxMs]` =
`[4500, 10000]` ms). See `documentation/Current_Architecture/PACKET_RELIABILITY.md`.

TDMA state is managed by `TdmaClock`. The session clock is:

```
sessionNow = syncSessionMs + (millis() - syncLocalMs)
```

Updated on every `TdmaClock::applySync()` call (triggered by incoming TIME_SYNC).

### TX queue and packet freshness

`TdmaTxQueue` holds 8 entries (`NetworkConfig::kQueueDepth`, capped by
`kQueueCapacityHardCap`). When full, the oldest entry is evicted — the queue always
holds the freshest data. `TdmaRadioService::drainTxQueue()` sends one payload per slot.

---

## Boot handshake + TIME_SYNC flow

```
Node Feather — SmartFiresNodeApp::begin()
  │  Broadcasts PKT_AWAKEN (9 bytes) to base station address
  │  Re-broadcasts every 5 s while waiting
  │  Sensors and duty cycle are HELD OFF until sync is received
  ▼
Base Feather  ← SmartFiresBaseApp (fully ported)
  │  Relays all node packets to Jetson over USB (encodeBaseFrame)
  │  Assigns node_id from uid_hash on first AWAKEN (findOrCreateNodeAssignment)
  │  Receives TIME_SYNC + command frames from the Jetson USB link, routes/broadcasts over LoRa
  │  Broadcasts 13-byte LoRa TIME_SYNC to RH_BROADCAST_ADDRESS (every 10 min)
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
| --- | --- | --- |
| Class-based firmware architecture | **Done** | SmartFiresNodeApp, PacketHandler, TdmaRadioService, TdmaClock, TdmaTxQueue |
| Binary bundle protocol (PKT_BUNDLE) | **Done** | BinaryPacket.h, PacketHandler, encodeBundlePayload() |
| Delta frames | **Done** | Reference + 14 DeltaPayloads per bundle (12-byte compact delta) |
| PKT_STATUS periodic packet | **Done** | GPS + battery every 15 min; statusPacketReady() / takeStatusPacket() / resetStatusTimer() |
| AWAKEN boot handshake | **Done** | Node broadcasts PKT_AWAKEN every 5 s until TIME_SYNC received; sensors withheld until synced |
| SHT31 sensor wired end-to-end | **Done** | fillSnapshot() implemented |
| TDMA clock + slot gating | **Done** | TdmaClock, TdmaRadioService |
| TIME_SYNC binary decode | **Done** | TdmaRadioService uses BinaryPacket::decodeTimeSync() |
| Base station port | **Done** | SmartFiresBaseApp fully implemented: LoRa RX, UART framing, TIME_SYNC, ACK_SUMMARY, node assignment |
| edge-receiver packet bundle decode | **Done** | `smartfires_edge/packet.py` for 20-byte FullStatePayload + 12-byte deltas |
| Jetson anemometer integration | **Done** | `smartfires-edge receive` can poll ES-W302 and log `jetson_wind_mps` + `jetson_wind_dir_deg` |
| Sensor fillSnapshot() (Wind, GPS, SPS30, IMU) | **Done** | All four sensors implement `fillSnapshot()` |
| CMD_CALIBRATE/CMD_RESET/CMD_ACK | **Round-trip done; node action is log-only** | Wire protocol + ACK is implemented node↔base↔Jetson; node-side handling only logs + ACKs — DMP free-runs calibration already, and CMD_RESET does not yet actually reset the board (`reset_type` is decoded but unused) |
| Heading/calibration system | **Done** | DMP self-calibration on boot; heading carried in every STATUS packet |

Full details and design notes in `documentation/Completed_Plans/BINARY_PACKET_PIPELINE.md`.
Sizing and scaling math tables are in `documentation/Current_Architecture/BANDWIDTH_SCALING.md`.

---

## Heading and Calibration System

The ICM-20948 IMU uses the DMP 9DOF Rotation Vector (gyro + accel + mag fusion) to compute
heading on-chip. No Jetson-side calibration pipeline exists — the DMP self-calibrates via
figure-8 motion at boot. Heading is transmitted in each STATUS packet.

### How it works

**Calibration (on boot):**
Perform slow figure-8 motion with the sensor for ~30–60 seconds. The DMP compass
self-calibrates internally. Biases are RAM-only and lost on power cycle — recalibrate
after each reboot. (Future: save/restore biases to flash using `getBiasCPassX/Y/Z()`.)

**Normal operation:**

- Every PKT_STATUS (15 min) carries `heading_deg_x10` and `heading_accuracy` (Q12 degrees).
- Jetson reads heading directly from the decoded STATUS packet. No calibration math needed.
- Heading is stored in `node_status["heading_true_deg"]` in `session.json` and surfaced via the
  web dashboard (`smartfires-edge web`) and the `visualize` terminal view.

**Node identity:** `uid_hash` (32-bit FNV-1a of the SAMD21 128-bit serial). Present in
every AWAKEN payload. Used as the node identity key on the Jetson. The base station
assigns `node_id` from this hash in `findOrCreateNodeAssignment()`.

**Expected accuracy:** ±2–5° when calibrated; raw Q12 accuracy field gives the DMP's own
estimate (divide by 4096 for degrees). Magnetic declination correction is implemented in
`session.py`'s `_location_corrected_heading()` — given a valid GPS fix, it looks up declination
via the `geomag` package (a hard dependency in `pyproject.toml`) and stores
`location_corrected_heading` alongside the raw `heading_true_deg`; it returns `None` if GPS is
invalid (and degrades gracefully if `geomag` is somehow missing at runtime).

---

## Edge Receiver (Jetson)

`smartfires-edge` (entry point in `pyproject.toml`, `main.py:main()`) has four subcommands:

```bash
pip install -e edge/edge-receiver

smartfires-edge receive --port /dev/smartfires-base --data-dir /mnt/nvme_drive/data
smartfires-edge receive --sync-interval 600      # 10-min sync interval
smartfires-edge receive --anemometer-port /dev/ttyUSB0 --anemometer-baud 9600 --anemometer-address 1

smartfires-edge web --port /dev/smartfires-base --http-port 8000          # ingest + FastAPI dashboard
smartfires-edge web --sniffer-port /dev/ttyUSB1 --num-slots 4             # + passive sniffer feed

smartfires-edge visualize --port /dev/smartfires-base    # live terminal telemetry/status tables
smartfires-edge summary --data-dir /mnt/nvme_drive/data  # packet-loss summary from saved state
```

`web` runs ingest (and, if `--sniffer-port` is given, the sniffer) on background threads and serves
the dashboard on the main thread via uvicorn: `/` (live map), `/map-history`, `/sniffer`, `/debug`,
`/live-log`, backed by a REST API (`/api/nodes`, `/api/telemetry/...`, `/api/base_station`,
`/api/command`, …) and WebSocket streams (`/ws/log`, `/ws/base-debug`, `/ws/sniffer`). **There is
currently no working way to send CMD_CALIBRATE/CMD_RESET from the Jetson** — `/api/command` is a
stub that echoes the request without writing to serial, and `sniffer_service.py` only passively
decodes these packet types for monitoring.

`edge/edge-receiver/src/smartfires_edge/packet.py` mirrors `BinaryPacket.h` for
STATUS/FULL_STATE/BUNDLE/CMD_CALIBRATE/CMD_RESET/CMD_ACK parsing and bundle delta expansion.

Base station link (USB, not UART — see `UART_JETSON_BRIDGE.md`):
1. Plug the base station Feather's USB cable into the Jetson.
2. One-time udev setup so the base and sniffer (both generic USB-CDC, same
   VID/PID) get stable, non-swapping device paths — see
   `JETSON_CHEATSHEET.md`'s "One-time udev setup".
3. Resulting device path: `/dev/smartfires-base` (symlink, not a raw
   `/dev/ttyACM*` path).

---

## Key Design Decisions

- **Single-board node:** Feather M0 owns both sensing and LoRa. No separate ESP32. Simplifies firmware, eliminates UART bridge between boards.
- **Binary not text:** text CSV was ~90 bytes/packet; binary bundle ≤194 bytes carries 15 samples. Effective per-sample cost ~13 bytes vs ~90 bytes.
- **Fixed-point integers on the wire:** no floats — deterministic size, no locale/printf issues on MCUs. `SensorSnapshot` uses floats internally; `PacketHandler::quantize()` converts.
- **SensorSnapshot as internal currency:** sensors write float readings into `SensorSnapshot` via `ISensor::fillSnapshot()`. Battery data is also written into the snapshot by `SmartFiresNodeApp`. `PacketHandler` owns all quantization and encoding — sensors never touch wire format.
- **AWAKEN boot handshake:** node withholds sensing until it receives a TIME_SYNC from the base. This ensures `session_time` in every bundle is valid from the first sample. Node re-broadcasts AWAKEN every 5 s until synced.
- **PKT_STATUS every 15 min:** replaces the old one-shot PKT_GPS. Carries GPS position + battery level with validity flags so the receiver always knows what's populated. First STATUS goes out on the first sensing cycle after sync.
- **TDMA slot assignment is compile-time:** `slot = (NODE_ID - 1) % NUM_SLOTS`. No runtime negotiation. Adding a node = change `NUM_SLOTS` to `(node count) + 1` in platformio.ini, reflash all Feathers.
- **Slot 0 is permanently reserved for the base station.** Node IDs are assigned starting at `kFirstNodeId = 2` (`config/BaseConfig.h`), so node 1/slot 0 is never given to a real node. The base enforces this with its own `TdmaClock` (`nodeId=1`, self-clocked from its own session time) and defers every outgoing transmission — `TIME_SYNC`, `ACK_SUMMARY`, `CMD_CALIBRATE`/`CMD_RESET` — until that window opens, instead of sending immediately at an arbitrary phase.
- **`NUM_SLOTS` must match across all node Feathers, and the base.** Mismatch causes slot collisions.
- **Drop-oldest queue:** `TdmaTxQueue` (8 entries) always holds the freshest data. No blocking between sensing and TX.
- **Stale-sync fallback:** if no TIME_SYNC for 22 min (2× the 10-min broadcast interval), `TdmaClock::myTurn()` returns true unconditionally — node transmits immediately rather than going silent.
- **TIME_SYNC driven by Jetson NTP, not GPS.** GPS PPS sync deferred; current crystal drift between synced nodes is within the 20 ms guard band.
