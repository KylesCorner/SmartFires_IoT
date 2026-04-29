# SmartFires — Software Design Document

## Table of Contents

1. [Overview](#1-overview)
2. [System Topology](#2-system-topology)
3. [Repository Layout](#3-repository-layout)
4. [Build Environments](#4-build-environments)
5. [ESP32 Drone Firmware](#5-esp32-drone-firmware)
   - 5.1 [Component Map](#51-component-map)
   - 5.2 [Main Loop Data Flow](#52-main-loop-data-flow)
   - 5.3 [Class Responsibilities](#53-class-responsibilities)
6. [Feather M0 Firmware](#6-feather-m0-firmware)
   - 6.1 [NODE Mode](#61-node-mode)
   - 6.2 [BASE STATION Mode](#62-base-station-mode)
7. [Wire Protocol](#7-wire-protocol)
   - 7.1 [UART Frame: Drone → Feather Node](#71-uart-frame-drone--feather-node)
   - 7.2 [LoRa Payload: Feather Node → Feather Base](#72-lora-payload-feather-node--feather-base)
   - 7.3 [UART Frame: Feather Base → Jetson](#73-uart-frame-feather-base--jetson)
   - 7.4 [PktHeader](#74-pktheader)
   - 7.5 [FullStatePayload](#75-fullstatepayload)
   - 7.6 [CRC-8/MAXIM](#76-crc-8maxim)
   - 7.7 [UART ACK: Feather Node → Drone](#77-uart-ack-feather-node--drone)
8. [End-to-End Data Pipeline](#8-end-to-end-data-pipeline)
9. [LoRa ACK and Retry](#9-lora-ack-and-retry)
   - 9.1 [RHReliableDatagram](#91-rhreliabledatagram)
   - 9.2 [Address Scheme](#92-address-scheme)
   - 9.3 [NODE_ID vs BASE_ADDR Collision Note](#93-node_id-vs-base_addr-collision-note)
   - 9.4 [Retry Logging](#94-retry-logging)
10. [Edge Receiver (Jetson)](#10-edge-receiver-jetson)
11. [Protocol Phases Roadmap](#11-protocol-phases-roadmap)
12. [Open Design Decisions](#12-open-design-decisions)

---

## 1. Overview

SmartFires is a wildfire IoT sensor network. Remote drone nodes collect environmental data (temperature, humidity, wind speed, flame detection, GPS, altitude via LIDAR, and IMU orientation) and transmit it wirelessly over LoRa 915 MHz to a fixed base station. The base station forwards all received telemetry over UART to a Jetson Orin Nano edge computer, which logs it to CSV for real-time analysis.

The protocol is binary, fixed-point, and deterministic — no floats on the wire, no text parsing. All framing uses a two-byte magic header, a length byte, and a CRC-8/MAXIM checksum.

---

## 2. System Topology

```
[ESP32 — Arduino Nano ESP32]
    Sensors: FlameSensor, WindSensorRevC, Sht31Sensor (temp/humidity),
             Pa1010dGpsSensor, LidarLiteV3, Icm20948Imu
    Actuators: OledDisplay, MatrixKeypadSensor
    |
    | UART (Serial1, 115200 baud, binary frames, 35 bytes)
    | ← ACK,<seq>\n  (text, Feather → ESP32)
    v
[Adafruit Feather M0 RFM95 — NODE side]
    RHReliableDatagram: up to 5 retries, 300 ms/attempt
    |
    | LoRa 915 MHz (RadioHead RHReliableDatagram, 13 dBm)
    | ← ACK packet  (auto-sent by base recvfromAck)
    v
[Adafruit Feather M0 RFM95 — BASE STATION side]
    |
    | UART (Serial1, 115200 baud, binary frames, 36 bytes with RSSI)
    v
[Jetson Orin Nano]
    edge/edge-reciever/src/smartfires_edge/ingest_service.py → telemetry CSV
```

Multiple nodes share the same 915 MHz channel. Each node has a unique `NODE_ID` baked in at compile time. Collision avoidance is currently staggered TX intervals with random jitter (Phase 1 approach). TDMA is planned for Phase 2 once TIME_SYNC is live.

---

## 3. Repository Layout

```
SmartFires_IoT/
├── CLAUDE.md                          Claude Code context (keep up to date)
├── SOFTWARE_DESIGN.md                 This document
├── platformio/                        PlatformIO project — all embedded firmware
│   ├── platformio.ini                 Build environments
│   ├── TELEMETRY_REWORK_PLAN.md       Full protocol design rationale
│   ├── src/
│   │   ├── shared/                    Headers included by all targets
│   │   │   ├── BinaryPacket.h         Wire format: structs, CRC, frame encoders ← main protocol file
│   │   │   ├── TelemetryPacket.h      Internal ESP32 sensor data struct (floats, not sent on wire)
│   │   │   ├── TelemetryCodec.h       Legacy text/CSV codec (unused on wire, kept for reference)
│   │   │   └── UartLoRaBridge.h       UART bridge: line reader, ACK parser, binary frame sender
│   │   ├── drone/                     ESP32 firmware
│   │   │   ├── main.cpp               Wires together all objects, calls app.setup() / app.loop()
│   │   │   ├── platform/
│   │   │   │   └── ArduinoClock.h     IClock implementation wrapping millis()
│   │   │   ├── app/
│   │   │   │   ├── DroneApp.h/.cpp    Top-level coordinator — owns the main loop
│   │   │   │   ├── DroneContext.h     Struct holding references to all hardware objects
│   │   │   │   ├── DroneState.h       Plain data structs: AppState, LinkState, UiState
│   │   │   │   ├── SensorManager.h/.cpp    Calls beginAll / sampleAll on sensor registry
│   │   │   │   ├── ActuatorManager.h/.cpp  Calls beginAll / updateAll on actuator registry
│   │   │   │   ├── TelemetryService.h/.cpp Reads DroneContext, builds TelemetryPacket
│   │   │   │   ├── LinkService.h/.cpp      Encodes + sends UART frames, handles ACK state
│   │   │   │   ├── KeypadController.h/.cpp Handles keypad input → UI/state changes
│   │   │   │   └── OledPageController.h/.cpp  OLED page rendering (Env, GPS, IMU, UART, LoRa, Lidar)
│   │   │   └── (sensor driver impls in src/drone/*.cpp)
│   │   └── lora_feather/
│   │       └── main.cpp               Feather firmware: NODE and BASE modes in one file
│   └── include/                       Sensor/actuator driver headers (ESP32 targets only)
│       ├── ISensor.h / IActuator.h / II2CDevice.h   Interfaces
│       ├── FlameSensor.h              Analog + digital flame sensor (header-only)
│       ├── WindSensorRevC.h           Rev C wind sensor (header-only)
│       ├── Sht31Sensor.h / .cpp       I2C temp/humidity
│       ├── Pa1010dGpsSensor.h / .cpp  I2C GPS (PA1010D)
│       ├── Icm20948Imu.h              9-DoF IMU (header + SparkFun lib)
│       ├── LidarLiteV3.h / .cpp       I2C LIDAR distance sensor
│       ├── OledDisplay.h              I2C OLED (U8g2)
│       ├── MatrixKeypadSensor.h       4×3 matrix keypad
│       └── PinMapping.h               ESP32 pin assignments
├── edge/                              Jetson Python code (not part of PlatformIO)
│   ├── receiver.py                    UART frame receiver → CSV writer
│   ├── packet.py                      Python mirror of BinaryPacket.h (decode only)
│   └── requirements.txt               pyserial>=3.5
└── lora/                              Legacy experimental LoRa sketches (ignore)
```

---

## 4. Build Environments

All commands run from `platformio/`.

| Environment | Board | Build flags | Purpose |
|---|---|---|---|
| `drone` | Arduino Nano ESP32 | `NODE_ID=1` | Sensor node 1 firmware |
| `drone_node2` | Arduino Nano ESP32 | `NODE_ID=2` | Sensor node 2 firmware |
| `lora_feather` | Feather M0 | `LORA_NODE=1 NODE_ID=1` | Radio node side, paired with `drone` |
| `lora_feather_node2` | Feather M0 | `LORA_NODE=1 NODE_ID=2` | Radio node side, paired with `drone_node2` |
| `lora_feather_base` | Feather M0 | `LORA_BASE_STATION=1` | Radio base station side |

```bash
pio run -e drone               --target upload   # flash ESP32 node 1
pio run -e lora_feather        --target upload   # flash Feather node 1
pio run -e lora_feather_base   --target upload   # flash Feather base station
pio device monitor -e drone                      # serial monitor ESP32
```

Adding a third node: duplicate `drone` and `lora_feather` envs in `platformio.ini`, set `NODE_ID=3` in both. That is the only required change.

---

## 5. ESP32 Drone Firmware

### 5.1 Component Map

```
main.cpp
├── Hardware objects (static, anonymous namespace)
│   ├── WindSensorRevC        wind
│   ├── Icm20948Imu           imu
│   ├── FlameSensor           flame
│   ├── Sht31Sensor           sht31
│   ├── Pa1010dGpsSensor      gps
│   ├── LidarLiteV3           lidar
│   ├── MatrixKeypadSensor    keypad
│   ├── OledDisplay           oled
│   └── UartLoRaBridge        bridge  (wraps HardwareSerial/Serial1)
│
├── DroneContext               references to all hardware objects
│                              + ISensor[] and IActuator[] registries
│
├── AppState                   link state (seq, ack, timing) + UI state
│
├── ArduinoClock               millis() wrapper implementing IClock
│
├── SensorManager              iterates sensor registry
├── ActuatorManager            iterates actuator registry
├── TelemetryService           builds TelemetryPacket from DroneContext
├── LinkService                encodes + sends UART frames, manages ACK state
├── KeypadController           maps keypad presses to state changes
├── OledPageController         renders OLED pages from AppState + DroneContext
│
└── DroneApp                   owns setup() and loop(); calls all of the above
```

### 5.2 Main Loop Data Flow

Every iteration of `DroneApp::loop()`:

```
1. LinkService::update()
      UartLoRaBridge reads Serial1 line-by-line
      Parses ACK,<seq>  →  updates LinkState.waitingForAck / lastAckedSeq
      Parses ERR,<msg>  →  logs error

2. SensorManager::sampleAll()       (skipped if !sensingEnabled)
      Calls each ISensor::sample()
      Each sensor updates its internal hasReading() / value caches

3. ActuatorManager::updateAll()
      Calls each IActuator::update()
      OledDisplay is the only actuator currently

4. KeypadController::update()
      Reads MatrixKeypadSensor
      Handles page-flip and enable/disable sensing

5. LinkService::maybeSendTelemetry(nodeId)
      Gate: sensingEnabled && elapsed >= kTelemetryPeriodMs (250 ms)
      Gate: !waitingForAck  (one in-flight packet at a time)
      TelemetryService::build()  →  TelemetryPacket  (floats, internal)
      Quantize to FullStatePayload  (fixed-point integers, wire format)
      BinaryPacket::encodeFullStateFrame()  →  35-byte UART frame
      UartLoRaBridge::sendBinaryFrame()  →  Serial1.write()
      Sets waitingForAck = true, records lastSentSeq and lastSendTimeMs

6. LinkService::handleAckTimeout()
      If waitingForAck && elapsed >= kAckTimeoutMs (300 ms):
          Clears waitingForAck, logs timeout
          (no retry on the ESP32 side — Feather handles LoRa retry)

7. OledPageController::render()
      Renders the current OLED page if display is healthy
```

### 5.3 Class Responsibilities

| Class | File | Responsibility |
|---|---|---|
| `DroneApp` | `app/DroneApp.h/.cpp` | Top-level coordinator. Owns setup and loop. Delegates everything. |
| `DroneContext` | `app/DroneContext.h` | Dependency injection container. Holds references to all hardware. |
| `AppState` / `LinkState` / `UiState` | `app/DroneState.h` | Plain data. No logic. Shared across services. |
| `SensorManager` | `app/SensorManager.h/.cpp` | Calls `beginAll()` and `sampleAll()` on the `ISensor*[]` registry. |
| `ActuatorManager` | `app/ActuatorManager.h/.cpp` | Calls `beginAll()` and `updateAll()` on the `IActuator*[]` registry. |
| `TelemetryService` | `app/TelemetryService.h/.cpp` | Reads sensor values from `DroneContext`, packages into `TelemetryPacket` (float struct, internal only). |
| `LinkService` | `app/LinkService.h/.cpp` | Owns the send/ACK state machine. Calls `TelemetryService::build()`, quantizes to wire format, sends. |
| `UartLoRaBridge` | `shared/UartLoRaBridge.h` | Low-level UART abstraction. Line reader + ACK parser + binary frame sender. |
| `KeypadController` | `app/KeypadController.h/.cpp` | Translates keypad events to `AppState` changes. |
| `OledPageController` | `app/OledPageController.h/.cpp` | Renders OLED pages: Env, GPS, IMU, LIDAR, UART stats, LoRa stats. |
| `ArduinoClock` | `platform/ArduinoClock.h` | Implements `IClock` by wrapping `millis()`. Allows time injection for testing. |

#### Internal telemetry type — TelemetryPacket

`TelemetryPacket` (defined in `shared/TelemetryPacket.h`) is an internal-only struct that uses native C++ types (floats, doubles, bool). It is **never sent on the wire**. `LinkService` quantizes it into `FullStatePayload` (fixed-point integers) before encoding.

```
TelemetryPacket  →  FullStatePayload quantization (LinkService::sendTelemetryFrame)
    windMps  × 100      → wind_cms     (uint16_t, cm/s)
    tempC    × 100      → temp_cdegc   (int16_t,  centi-°C)
    humidity × 100      → humidity_cpct (uint16_t, centi-%)
    lat      × 1e7      → lat_e7       (int32_t)
    lon      × 1e7      → lon_e7       (int32_t)
```

---

## 6. Feather M0 Firmware

Both operating modes live in a single file (`src/lora_feather/main.cpp`) and are selected at compile time by preprocessor flags.

### 6.1 NODE Mode

**Build flag:** `-D LORA_NODE=1 -D NODE_ID=<n>`

Responsibilities:
- Receive 35-byte binary UART frames from the ESP32 over `Serial1`
- Validate CRC-8/MAXIM
- Send `ACK,<seq>\n` back to ESP32 immediately over `Serial1`
- Forward the 31-byte LoRa payload (header + sensor data) to the base station using `RHReliableDatagram::sendtoWait()`
- Retry up to 5 times (300 ms per attempt) if no ACK received from base
- Log per-packet retry count and success/failure to USB Serial

Frame receiver state machine states: `FS_WAIT_M0 → FS_WAIT_M1 → FS_WAIT_LEN → FS_READ_DATA → FS_CHECK_CRC`

Key timing notes:
- The UART ACK to ESP32 is sent **before** `sendtoWait()` starts. The ESP32 is not aware of LoRa outcome.
- `sendtoWait()` can block up to `LORA_RETRIES × LORA_TIMEOUT_MS` = 5 × 300 ms = **1.5 seconds** in the worst case. During this time, new UART bytes from ESP32 accumulate in the hardware RX buffer. They are processed in the next `loop()` iteration after `sendtoWait()` returns.

### 6.2 BASE STATION Mode

**Build flag:** `-D LORA_BASE_STATION=1`

Responsibilities:
- Listen for LoRa packets from any node using `RHReliableDatagram::recvfromAck()`
- `recvfromAck()` automatically sends an ACK back to the sender — no manual ACK handling needed
- Validate packet magic and type before forwarding
- Prepend RSSI as `int8_t` and build a 36-byte UART frame
- Write the frame to `Serial1` for the Jetson to consume

---

## 7. Wire Protocol

All multi-byte integers are **little-endian**. No floats anywhere on the wire.

### 7.1 UART Frame: Drone → Feather Node

Total: **35 bytes**

```
Offset  Size  Field
------  ----  -----
0       1     0xAA  (FRAME_M0, sync byte 1)
1       1     0x55  (FRAME_M1, sync byte 2)
2       1     0x1F  (len = 31, data length in bytes)
3       4     PktHeader
7       27    FullStatePayload
34      1     CRC-8/MAXIM  (covers byte[2..33] — len byte + data bytes)
```

### 7.2 LoRa Payload: Feather Node → Feather Base

Total: **31 bytes** — no extra framing, RadioHead handles its own framing on the radio link.

```
Offset  Size  Field
------  ----  -----
0       4     PktHeader
4       27    FullStatePayload
```

### 7.3 UART Frame: Feather Base → Jetson

Total: **36 bytes**

```
Offset  Size  Field
------  ----  -----
0       1     0xAA  (FRAME_M0)
1       1     0x55  (FRAME_M1)
2       1     0x20  (len = 32, data length: 1 byte RSSI + 31 bytes LoRa payload)
3       1     RSSI  (int8_t, dBm as seen by base station radio)
4       4     PktHeader
8       27    FullStatePayload
35      1     CRC-8/MAXIM  (covers byte[2..34])
```

### 7.4 PktHeader

4 bytes, packed.

| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | `uint8_t` | `magic` | `0xA5` — always, used for validation |
| 1 | `uint8_t` | `pkt_type` | `0x01` = FULL_STATE, `0x02` = HEARTBEAT, `0x03` = TIME_SYNC (reserved) |
| 2 | `uint8_t` | `node_id` | Compile-time `NODE_ID` |
| 3 | `uint8_t` | `seq` | Rolling 0–255, wraps around |

### 7.5 FullStatePayload

27 bytes, packed, little-endian.

| Offset | Type | Field | Encoding | Raw unit |
|---|---|---|---|---|
| 0 | `uint32_t` | `session_time` | ms — local `millis()` until TIME_SYNC is live | ms |
| 4 | `uint32_t` | `uptime_ms` | `millis()` since boot | ms |
| 8 | `uint16_t` | `sensor_flags` | Bitmask: `FLAME=0x01 WIND=0x02 SHT31=0x04 LIDAR=0x08 GPS=0x10 IMU=0x20` | — |
| 10 | `uint8_t` | `flame` | 0 or 1 | — |
| 11 | `uint16_t` | `wind_cms` | `windMps × 100` | cm/s |
| 13 | `int16_t` | `temp_cdegc` | `tempC × 100` | centi-°C |
| 15 | `uint16_t` | `humidity_cpct` | `humidityPct × 100` | centi-% |
| 17 | `uint16_t` | `lidar_cm` | distance | cm |
| 19 | `int32_t` | `lat_e7` | `latitude × 1e7` | degrees×1e7 |
| 23 | `int32_t` | `lon_e7` | `longitude × 1e7` | degrees×1e7 |

A set bit in `sensor_flags` means that sensor was healthy and produced a valid reading for this packet. If the bit is clear, the corresponding field value is undefined/default-zero and should be ignored by the receiver.

### 7.6 CRC-8/MAXIM

Polynomial: `0x31`. Initial value: `0x00`. Covers the `len` byte plus all data bytes (does not cover the two sync bytes).

The same algorithm is implemented in both C++ (`BinaryPacket::crc8()`) and Python (`packet.crc8()`).

### 7.7 UART ACK: Feather Node → Drone

Text line, sent over `Serial1` from the Feather node back to the ESP32.

```
ACK,<seq>\n
```

Where `<seq>` is the 8-bit sequence number from `PktHeader.seq`. The ESP32 parses this in `UartLoRaBridge::handleLine()` and clears `LinkState.waitingForAck` when the sequence number matches.

On boot, the Feather sends `ACK,BOOT\n` — `UartLoRaBridge` detects this and sets `bootSeen = true`.

---

## 8. End-to-End Data Pipeline

```
Physical sensors
    │  analog/I2C/UART reads
    ▼
ISensor::sample()          — each sensor caches its latest reading internally
    │
    ▼
TelemetryService::build()  — reads sensor caches, produces TelemetryPacket
                             (float/double, internal only, never on wire)
    │  quantize to fixed-point
    ▼
FullStatePayload           — 27-byte packed struct
    │
    ▼
BinaryPacket::encodeFullStateFrame()
                           — prepends sync bytes + PktHeader, appends CRC
                           — produces 35-byte UART frame
    │
    ▼
UartLoRaBridge::sendBinaryFrame()
                           — Serial1.write() to Feather node
    │  UART 115200 baud
    ▼
Feather NODE: CRC validated, ACK sent to ESP32
    │
    ▼
RHReliableDatagram::sendtoWait()
                           — sends 31-byte LoRa payload (PktHeader + FullStatePayload)
                           — retries up to 5× if no ACK received
    │  LoRa 915 MHz
    ▼
Feather BASE: recvfromAck()
                           — receives packet, sends ACK automatically
                           — validates magic + pkt_type
    │
    ▼
BinaryPacket::encodeBaseFrame()
                           — prepends sync bytes, RSSI, appends CRC
                           — produces 36-byte UART frame
    │
    ▼
Serial1.write() to Jetson  — UART 115200 baud
    │
    ▼
smartfires_edge/uart_receiver.py   — state-machine frame parser
    │
    ▼
packet.decode_full_state() — unpack struct, scale back to floats
    │
    ▼
telemetry.csv              — one row per packet
    columns: timestamp, node_id, seq, session_time_ms, uptime_ms,
             sensor_flags, flame, wind_mps, temp_c, humidity_pct,
             lidar_cm, lat, lon, rssi
```

---

## 9. LoRa ACK and Retry

### 9.1 RHReliableDatagram

The Feather firmware uses RadioHead's `RHReliableDatagram` layer on top of the raw `RH_RF95` driver. This replaces the original fire-and-forget `rf95.send()` approach, which produced approximately 40% packet loss with no recovery.

**NODE side** (`sendtoWait`):
- Sends the 31-byte payload to `BASE_ADDR`
- Blocks internally, re-sending up to `LORA_RETRIES` times if no ACK arrives within `LORA_TIMEOUT_MS`
- Returns `true` if ACK received, `false` if all retries exhausted

**BASE side** (`recvfromAck`):
- Polls for an available packet
- When a packet arrives, automatically sends a RadioHead ACK frame back to the sender
- Returns `true` and fills the buffer when a valid packet is ready for the application layer

Configured constants (in `lora_feather/main.cpp`):

| Constant | Value | Meaning |
|---|---|---|
| `LORA_RETRIES` | 5 | Max retry attempts per packet |
| `LORA_TIMEOUT_MS` | 300 ms | Timeout per attempt waiting for ACK |
| Worst-case block time | 1500 ms | 5 × 300 ms — only if all 5 retries fail |

### 9.2 Address Scheme

`RHReliableDatagram` requires each device to have a unique 8-bit address.

| Device | Address | How set |
|---|---|---|
| Base station | `0x01` (`BASE_ADDR`) | Compile-time constant in `main.cpp` |
| Node 1 | `0x01` (= `NODE_ID`) | `-D NODE_ID=1` in `platformio.ini` |
| Node 2 | `0x02` (= `NODE_ID`) | `-D NODE_ID=2` in `platformio.ini` |
| Node N | `NODE_ID` | `-D NODE_ID=N` in `platformio.ini` |

### 9.3 NODE_ID vs BASE_ADDR Collision Note

With the current single-node setup, `NODE_ID=1` and `BASE_ADDR=0x01` are numerically the same value. This is fine. `RHReliableDatagram` uses the `from` address embedded in each RadioHead frame to route ACKs back to the correct sender — it does not confuse node 1 with the base station because they are on opposite sides of each exchange (the node sends, the base receives and ACKs, never vice versa in the current architecture).

When adding node 2, set `NODE_ID=2` in the `lora_feather_node2` and `drone_node2` envs. Its RHReliableDatagram address becomes `0x02`, which is distinct from the base station at `0x01`. The base station requires no change — it accepts packets from any sender address.

### 9.4 Retry Logging

The node prints to USB Serial for every packet:

```
[LORA TX] seq=42 node=1 sending...
[LORA TX] seq=42 ACKed                        ← delivered first attempt
[LORA TX] seq=43 ACKed after 2 retries        ← delivered after 2 retransmissions
[LORA TX] seq=44 FAILED after 5 retries — packet dropped
```

The retry count is derived from `manager.retransmissions()` delta (before vs after each `sendtoWait()` call).

---

## 10. Edge Receiver (Jetson)

The Python receiver runs on the Jetson Orin Nano and consumes the 36-byte UART frames from the Feather base station.

```bash
# Install dependency
pip install -e edge/edge-reciever

# Run (adjust --port as needed)
smartfires-edge receive --port /dev/ttyTHS1 --data-dir /mnt/nvme_drive/data
```

### Jetson UART one-time setup

```bash
sudo /opt/nvidia/jetson-io/jetson-io.py           # enable UART pin group
sudo systemctl disable nvgetty && sudo udevadm trigger   # free port from serial console
# Typical device path: /dev/ttyTHS1
```

### Frame parser state machine

States: `WAIT_M0 → WAIT_M1 → WAIT_LEN → READ_DATA → CHECK_CRC`

On CRC match, it calls `packet.decode_full_state()` which unpacks the struct and scales all fixed-point fields back to floats/doubles. One CSV row is written per valid packet and the file is flushed immediately.

### CSV columns

| Column | Source | Notes |
|---|---|---|
| `timestamp` | Jetson system clock (UTC) | ISO-8601 with milliseconds |
| `node_id` | PktHeader.node_id | Which drone node sent this |
| `seq` | PktHeader.seq | 8-bit rolling sequence number |
| `session_time_ms` | FullStatePayload.session_time | Local millis() until Phase 2 TIME_SYNC |
| `uptime_ms` | FullStatePayload.uptime_ms | ms since ESP32 boot |
| `sensor_flags` | FullStatePayload.sensor_flags | Bitmask — which fields are valid |
| `flame` | FullStatePayload.flame | Boolean |
| `wind_mps` | wind_cms / 100 | m/s |
| `temp_c` | temp_cdegc / 100 | °C |
| `humidity_pct` | humidity_cpct / 100 | % |
| `lidar_cm` | FullStatePayload.lidar_cm | cm |
| `lat` | lat_e7 / 1e7 | decimal degrees |
| `lon` | lon_e7 / 1e7 | decimal degrees |
| `rssi` | UART frame rssi byte | dBm at base station radio |

---

## 11. Protocol Phases Roadmap

| Phase | Status | Summary |
|---|---|---|
| 1 — Binary full-state | Done | Replaced text CSV with packed binary frames end-to-end |
| 1b — LoRa ACK/retry | Done | `RHReliableDatagram` with 5 retries/300 ms per attempt |
| 2 — TIME_SYNC | Not started | Base station broadcasts authoritative `session_time`. Nodes maintain a `millis()` offset. `PKT_TIME_SYNC = 0x03` already reserved in `BinaryPacket.h` |
| 3 — Delta packets | Not started | Changed-field bitmask, per-field thresholds, periodic full-state refresh cadence |
| 4 — Feather TX queue + duty cycle | Not started | ESP32 batches packets during sensing interval. Feather wakes LoRa radio, sends full batch with per-packet ACK/retry (already in place via RHReliableDatagram), then sleeps radio to save power. Batch size is dynamic; ESP32 may send a metadata packet to Feather to signal batch start/count. |
| 5 — Receiver recovery | Not started | Sequence-gap handling on Jetson, optional `SYNC_REQUEST`, debug counters |

Full design rationale for all phases is in `platformio/TELEMETRY_REWORK_PLAN.md`.

---

## 12. Open Design Decisions

These are unresolved questions that will need answers before the corresponding phase is implemented.

| Question | Relevant phase |
|---|---|
| What is the full-state refresh cadence when delta packets are active? | 3 |
| What are the per-field change thresholds (wind, temp, humidity, GPS movement)? | 3 |
| Does the UART ACK from Feather mean "enqueued" or "sent over LoRa"? | 4 |
| What is the duty cycle period and how is it triggered (timer, ESP32 command, or both)? | 4 |
| How does the ESP32 signal the start of a batch to the Feather (metadata packet format)? | 4 |
| Does the Feather need a TX queue ring buffer, or is one-in-flight sufficient for initial duty cycle? | 4 |
| Does the base station send TIME_SYNC by broadcast only or also unicast per-node repair? | 2 / 5 |
| What is the sequence number width for delta packets — keep 8-bit or promote to 16-bit? | 3 |
| Should `RHReliableDatagram` timeout per retry be tuned based on observed LoRa airtime at the chosen spreading factor? | ongoing |
