---
name: documentation-index
description: Index of current SmartFires architecture and operator documentation.
category: index
status: current
last_verified: 2026-10-16
related_docs:
  - software-design
---

# SmartFires IoT documentation

This directory documents the system that is currently implemented. Shipped
code and `platformio/platformio.ini` remain authoritative when documentation
and implementation disagree.

## Architecture

| Document | Covers |
|---|---|
| [`SOFTWARE_DESIGN.md`](SOFTWARE_DESIGN.md) | End-to-end hardware, firmware, edge-service, packet, and command architecture |
| [`SOFTWARE_DESIGN_DIAGRAM.md`](SOFTWARE_DESIGN_DIAGRAM.md) | System, join, duty-cycle, command, and framing diagrams |
| [`Current_Architecture/TUNABLE_PARAMETERS.md`](Current_Architecture/TUNABLE_PARAMETERS.md) | Active TDMA, reliability, sensing, power, base, and edge values |
| [`Current_Architecture/TDMA_PROTOCOL.md`](Current_Architecture/TDMA_PROTOCOL.md) | Slot geometry, assignment, clocks, transmit budgets, and window markers |
| [`Current_Architecture/PACKET_RELIABILITY.md`](Current_Architecture/PACKET_RELIABILITY.md) | Link acknowledgements, application summaries, retries, and sleeping nodes |
| [`Current_Architecture/DUTY_CYCLING.md`](Current_Architecture/DUTY_CYCLING.md) | Continuous, SensorTriggered, Timed, and Hybrid behavior |
| [`Current_Architecture/JETSON_BRIDGE.md`](Current_Architecture/JETSON_BRIDGE.md) | USB framing, ingest, session lifecycle, API, and dashboard behavior |
| [`Current_Architecture/BANDWIDTH_SCALING.md`](Current_Architecture/BANDWIDTH_SCALING.md) | Bundle production, airtime, slot service, and scaling constraints |
| [`Current_Architecture/LORA_VS_LORAWAN.md`](Current_Architecture/LORA_VS_LORAWAN.md) | Raw-LoRa design choices and deployment tradeoffs |

## Operator references

| Document | Covers |
|---|---|
| [`User_Reference/FLASHING.md`](User_Reference/FLASHING.md) | Firmware targets, upload commands, and troubleshooting |
| [`User_Reference/DEBUG_FILTER.md`](User_Reference/DEBUG_FILTER.md) | Structured `@SFDBG` log format and monitor filters |
| [`User_Reference/JETSON_CHEATSHEET.md`](User_Reference/JETSON_CHEATSHEET.md) | Edge installation, udev links, services, and data operations |
| [`User_Reference/NETWORK_TEST.md`](User_Reference/NETWORK_TEST.md) | Hardware LoRa-to-Jetson integration test |
| [`User_Reference/LORA_SNIFFER.md`](User_Reference/LORA_SNIFFER.md) | Passive-sniffer firmware and dashboard integration |
| [`User_Reference/SMARTFIRES_MANAGER.md`](User_Reference/SMARTFIRES_MANAGER.md) | Jetson update, service, and gateway-flashing manager |
| [`POWER_MEASUREMENTS.md`](POWER_MEASUREMENTS.md) | Isolated power-test environments and measurement setup |

## Design tool

[`../tools/lora_tdma_calculator.html`](../tools/lora_tdma_calculator.html) is a
standalone calculator for LoRa airtime, effective bandwidth, TDMA capacity,
node count, and estimated energy use.

## Documentation maintenance

- [`DOC_FRONTMATTER.md`](DOC_FRONTMATTER.md) defines document metadata and
  source references.
- [`CODE_FRONTMATTER.md`](CODE_FRONTMATTER.md) defines reciprocal firmware
  source headers.
- `python3 documentation/check_doc_freshness.py` checks document metadata,
  source paths, and verification dates.
- `python3 documentation/check_code_headers.py` checks C++ metadata and
  document/source backlinks.

These checks establish structural consistency; claims still require review
against the implementation.
