# SmartFires IoT Software Design

## Overview

SmartFires IoT is a wildfire telemetry system built around remote Feather M0 LoRa nodes, a Feather M0 base station, and a Jetson Orin Nano edge receiver.

Each remote node samples attached sensors, converts readings into a common in-memory snapshot, compresses telemetry into deterministic binary packets, and transmits those packets over LoRa using TDMA slot timing. The base station bridges LoRa traffic to the Jetson over UART and rebroadcasts session clock updates from the Jetson back to all nodes.

The current firmware architecture is class-based and centered on narrow responsibilities:

- sensors populate `SensorSnapshot`
- the application coordinates sampling and packet production
- packet encoding is isolated from sensor code
- radio timing and queueing are isolated from telemetry creation
- the base station is a protocol bridge between LoRa and UART

## System Topology

```text
[Feather M0 LoRa node]
  SHT31, wind, GPS, SPS30, ICM-20948, battery monitor
  SmartFiresNodeApp
    -> DutyCycleController
    -> PacketHandler
    -> TdmaRadioService
    -> RadioHeadTdmaDriver
        |
        | LoRa 915 MHz
        v
[Feather M0 LoRa base station]
  SmartFiresBaseApp
    -> RadioHeadTdmaDriver
    -> UART bridge
        |
        | UART 115200
        v
[Jetson Orin Nano]
  smartfires_edge.ingest_service
    -> packet decode
    -> CSV logging
    -> TIME_SYNC generation
```

## Design Goals

- Keep node firmware deterministic and lightweight on constrained hardware.
- Keep wire formats compact and explicit.
- Separate sensor acquisition from packet encoding.
- Support multiple nodes on one LoRa channel with compile-time TDMA configuration.
- Preserve a clean test seam for native unit tests.
- Allow incremental sensor bring-up without redesigning the application skeleton.

## Repository Layout

```text
SmartFires_IoT/
├── platformio/
│   ├── platformio.ini
│   ├── include/
│   │   ├── app/
│   │   ├── drivers/
│   │   ├── interfaces/
│   │   ├── platform/
│   │   ├── power/
│   │   ├── radio/
│   │   ├── sensors/
│   │   └── telemetry/
│   ├── src/
│   │   ├── app/
│   │   ├── platform/
│   │   ├── power/
│   │   ├── radio/
│   │   ├── sensors/
│   │   └── telemetry/
│   └── test/
├── edge/
│   ├── anemometer_read.py
│   └── edge-receiver/
├── documentation/
│   ├── README.md                      — doc index
│   ├── SOFTWARE_DESIGN.md             — this file
│   ├── SOFTWARE_DESIGN_DIAGRAM.md
│   ├── Current_Architecture/          — subsystem deep-dives (current state)
│   │   ├── TDMA_PROTOCOL.md
│   │   ├── PACKET_RELIABILITY.md
│   │   ├── DUTY_CYCLING.md
│   │   ├── UART_JETSON_BRIDGE.md
│   │   └── BANDWIDTH_SCALING.md
│   ├── User_Reference/                — how-to guides
│   │   ├── FLASHING.md
│   │   ├── DEBUG_FILTER.md
│   │   ├── JETSON_CHEATSHEET.md
│   │   └── NETWORK_TEST.md
│   └── Completed_Plans/               — historical design docs
├── CLAUDE.md
└── README.md
```

## Runtime Architecture

### Node Firmware

The node firmware is assembled in `platformio/src/main.cpp`.

At startup, the program constructs:

- platform abstractions such as `ArduinoClock` and `ArduinoAnalogReader`
- concrete sensor drivers such as `AdafruitSht31Driver` and `AdafruitGpsDriver`
- sensor wrappers implementing `ISensor`
- `BatteryMonitor`
- `DutyCycleController`
- telemetry and radio components: `PacketHandler`, `TdmaClock`, `TdmaTxQueue`, `TdmaRadioService`, `RadioHeadTdmaDriver`
- `SmartFiresNodeApp` as the top-level coordinator

The high-level node loop is:

```text
setup()
  -> initialize serial and I2C
  -> construct dependencies
  -> SmartFiresNodeApp::begin()

loop()
  -> SmartFiresNodeApp::update()
  -> delay(25)
```

### SmartFiresNodeApp Responsibilities

`SmartFiresNodeApp` is responsible for application-level control flow only.

- initializes battery, duty-cycle logic, and radio services
- sends an `AWAKEN` packet on startup
- polls the radio service every loop so incoming `TIME_SYNC` frames can be processed
- advances the duty-cycle state machine
- builds a `SensorSnapshot` when telemetry is ready
- forwards snapshots to `PacketHandler`
- enqueues generated `STATUS` and `BUNDLE` packets to the radio service

`SmartFiresNodeApp` does not know binary field layouts beyond asking `PacketHandler` for encoded payloads.

### Sensor Model

Every sensor implementation writes into the shared `SensorSnapshot` structure via `ISensor::fillSnapshot(SensorSnapshot&)`.

This keeps sensor code independent from packet formats. Sensors report physical units in memory, such as:

- temperature in Celsius
- humidity in percent
- wind speed in meters per second
- particulate values in micrograms per cubic meter
- GPS coordinates in degrees

Battery readings are added by the application after sensor sampling.

### DutyCycleController

`DutyCycleController` owns the wake, sample, and sleep sequencing for sensors. Its job is to decide when a fresh telemetry snapshot should be emitted. The node app uses it as a gate rather than embedding timing rules directly in the application loop.

This separation lets sampling policy evolve without changing packet handling or radio orchestration.

### PacketHandler

`PacketHandler` is the node-side telemetry encoder.

Its responsibilities are:

- convert `SensorSnapshot` values into fixed-point representations
- track packet sequencing
- emit a periodic `STATUS` packet containing GPS and battery state
- accumulate one reference sample plus delta samples into a binary telemetry bundle
- return encoded packet bytes ready for LoRa transmission

This class is the boundary between internal sensor units and the on-air binary protocol.

### TDMA Radio Path

The LoRa transmission path is split into explicit layers:

- `TdmaClock` computes session time and determines slot timing from sync state
- `TdmaTxQueue` buffers outgoing payloads, dropping the oldest item when full
- `TdmaRadioService` processes incoming sync packets and drains the queue during allowed send windows
- `RadioHeadTdmaDriver` translates the abstract radio interface to RadioHead calls on Feather hardware

This keeps slot timing, buffering, and hardware access separated so each can be tested independently.

## Base Station Architecture

`SmartFiresBaseApp` is the top-level coordinator for the base station firmware.

Its runtime loop is simple by design:

- receive LoRa packets from nodes
- wrap them into UART frames with RSSI metadata
- forward them to the Jetson
- parse incoming Jetson UART commands
- forward `TIME_SYNC` broadcasts or targeted ACK-summary messages over LoRa
- log periodic health counters to the debug UART

The base station deliberately does not decode the full telemetry payload beyond the minimum needed to route commands. It behaves as a protocol bridge.

## Edge Receiver Architecture

The Jetson-side Python package in `edge/edge-receiver/src/smartfires_edge/` is responsible for:

- reading UART frames from the base station
- decoding binary payloads mirrored from `BinaryPacket.h`
- expanding bundle payloads into per-sample rows
- writing rotated CSV telemetry logs
- generating periodic `TIME_SYNC` command frames
- optionally merging local anemometer readings into output rows

The Python side mirrors the binary protocol rather than redefining it.

## Data Flow

### Node Data Flow

```text
ISensor::fillSnapshot()
  -> SensorSnapshot
  -> PacketHandler::push()
  -> BinaryPacket encode helpers
  -> TdmaTxQueue::enqueue()
  -> TdmaRadioService::update()
  -> RadioHeadTdmaDriver send/sendToWait
```

### Base Station Data Flow

```text
LoRa receive
  -> SmartFiresBaseApp::processIncomingLoRa()
  -> BinaryPacket::encodeBaseFrame()
  -> Jetson UART

Jetson UART command
  -> SmartFiresBaseApp::processIncomingJetsonUart()
  -> command payload validation
  -> LoRa broadcast or targeted send
```

## Wire Protocol Summary

The current protocol is binary and fixed-size where practical.

Node to base packets:

- `AWAKEN`: boot signal before the session is synchronized
- `STATUS`: GPS and battery summary
- `BUNDLE`: one full reference sample and multiple compact deltas

Base to node packets:

- `TIME_SYNC`: session ID and session time broadcast
- `ACK_SUMMARY`: targeted app-layer reliability summary generated by the base

Base to Jetson UART frames wrap received LoRa payloads with:

- frame magic bytes
- payload length
- RSSI byte
- payload bytes
- CRC-8/MAXIM

Detailed field layouts are maintained in `documentation/Completed_Plans/BINARY_PACKET_PIPELINE.md` and `platformio/include/telemetry/BinaryPacket.h`. For the current reliability model see `documentation/Current_Architecture/PACKET_RELIABILITY.md`.

## Build Targets

The current PlatformIO environments are:

| Environment | Purpose | Key flags |
| --- | --- | --- |
| `native` | host-based unit tests | `UNIT_TEST` |
| `feather_m0_lora_node` | real sensor node firmware | `LORA_NODE=2` `NUM_SLOTS=4` `SMARTFIRES_TDMA_RELIABILITY_MODE=1` `SMARTFIRES_STATUS_INTERVAL_MS=5000` `ICM_20948_USE_DMP` |
| `feather_m0_lora_node_debug` | debug node (default env, debug filter) | `LORA_NODE=1` `NUM_SLOTS=4` `SMARTFIRES_TDMA_RELIABILITY_MODE=1` `SMARTFIRES_STATUS_INTERVAL_MS=1000` `ICM_20948_USE_DMP` |
| `feather_m0_lora_base` | base station firmware | `LORA_BASE=1` |
| `feather_m0_lora_sniffer` | passive LoRa packet monitor | `SMARTFIRES_LORA_SNIFFER=1` |

Key compile-time flags:

- `NODE_ID` — overrides runtime uid_hash node identity for test nodes; real nodes derive identity from the SAMD21 serial number
- `NUM_SLOTS` — must match across all deployed node Feathers; mismatch causes slot collisions
- `LORA_NODE` / `LORA_BASE` — selects node vs base firmware role in `main.cpp`
- `SMARTFIRES_TDMA_RELIABILITY_MODE` — `0` = StrictLinkAck, `1` = AppLayerAckSummary (production default)

## Test Strategy

The primary firmware validation path is native unit testing in `platformio/test/`.

The suite uses fakes for time, sensors, radio, and analog reads so that packet assembly, duty-cycle logic, and application orchestration can be tested without hardware.

This is a deliberate architectural constraint: logic should remain behind interfaces where it can be exercised in the `native` environment.

## Current Status

Stable architectural pieces:

- class-based node application skeleton
- class-based base station application skeleton
- binary packet definitions and bundle encoding path
- TDMA timing and queue abstractions
- Jetson ingest and decode path
- native test structure

Still being wired into the finalized structure:

- several sensor implementations beyond the currently active SHT31 and GPS path in `main.cpp`
- full deployment-time hardware bring-up for all expected node sensors
- continued refinement of base station behavior as the bridge interface matures

## Extension Guidance

When adding a new sensor:

1. add or update the driver abstraction if hardware access is not already represented
2. implement or finish the `ISensor` wrapper in `platformio/include/sensors/` and `platformio/src/sensors/`
3. write values into `SensorSnapshot` only
4. wire the sensor into `platformio/src/main.cpp`
5. update packet encoding only if the wire protocol truly needs a new field
6. add or update native tests for the sensor and any affected packet logic

When changing the protocol:

1. update `BinaryPacket.h`
2. update node-side encoding in `PacketHandler`
3. update Python decode logic in `edge/edge-receiver/src/smartfires_edge/packet.py`
4. update the design docs and bandwidth notes

That preserves the current separation between sensing, transport, and ingest.
