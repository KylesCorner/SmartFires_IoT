# SmartFires IoT

SmartFires IoT is a wildfire telemetry system built around Feather M0 LoRa sensor nodes, a Feather M0 base station, and a Jetson Orin Nano edge receiver.

The embedded firmware is structured around a finalized class-based architecture. Sensor wiring is still being completed for a few devices, but the major application, telemetry, radio, and ingest boundaries are in place.

## Architecture Summary

- remote Feather nodes sample sensors and build `SensorSnapshot` data
- `PacketHandler` converts snapshots into compact binary telemetry packets
- `TdmaRadioService` sends packets over LoRa with TDMA slot timing
- the base station forwards LoRa packets to the Jetson over UART
- the Jetson decodes packets, expands bundles, logs CSV telemetry, and emits `TIME_SYNC`

## Repository Guide

```text
platformio/      Embedded firmware, interfaces, drivers, tests
edge/            Jetson ingest and anemometer utilities
documentation/   Design notes, protocol docs, flashing guidance
CLAUDE.md        Maintained project context and architecture summary
```

## Core Docs

- `documentation/SOFTWARE_DESIGN.md` describes the current software architecture
- `documentation/SOFTWARE_DESIGN_DIAGRAM.md` provides Mermaid diagrams for topology and control flow
- `documentation/BINARY_PACKET_PIPELINE.md` describes the wire protocol and packet pipeline
- `documentation/BANDWIDTH_SCALING.md` contains sizing and scaling notes
- `documentation/FLASHING.md` contains device flashing instructions

## Build and Test

Run embedded commands from `platformio/`.

```bash
pio run -e feather_m0_lora_node --target upload
pio run -e feather_m0_lora --target upload
pio device monitor -e feather_m0_lora_node
pio test -e native
```

## Current State

What is in place:

- class-based node and base station application structure
- binary packet definitions and bundle encoding
- TDMA timing, queueing, and radio driver abstraction
- Jetson-side UART ingest and packet decode
- native unit-test layout for firmware logic

What is still being finished:

- wiring additional sensors into the finalized node composition in `platformio/src/main.cpp`
- validating end-to-end hardware behavior as more sensors are enabled

## Development Notes

When making changes:

- keep sensor code writing into `SensorSnapshot`, not directly into wire structs
- keep protocol changes synchronized between C++ `BinaryPacket.h` and Python `packet.py`
- prefer adding tests in `platformio/test/` for application and packet behavior
- keep `CLAUDE.md` aligned with major architecture changes

## Entry Points

- node and base firmware composition: `platformio/src/main.cpp`
- node application coordinator: `platformio/src/app/SmartFiresNodeApp.cpp`
- base station coordinator: `platformio/src/app/SmartFiresBaseApp.cpp`
- Jetson ingest service: `edge/edge-reciever/src/smartfires_edge/ingest_service.py`

## License and Project Notes

No license file is currently present at the repository root. Add one explicitly if this repository is meant to be redistributed outside the current team.
