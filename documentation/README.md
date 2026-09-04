---
name: documentation-index
description: Table of contents and implementation-status ledger for documentation/.
category: index
status: current
last_verified: 2026-09-04
related_docs:
  - software-design
---

# SmartFires IoT documentation

Current architecture and reference docs were consolidated against the source tree on 2026-09-04. Use this index to distinguish shipped behavior from proposals and historical design records.

## Reading order

1. [`../AGENTS.md`](../AGENTS.md) for repository guardrails and agent-facing invariants.
2. [`SOFTWARE_DESIGN.md`](SOFTWARE_DESIGN.md) for the end-to-end system.
3. The relevant `Current_Architecture/` deep dive.
4. A `User_Reference/` guide for operational work.
5. Pending/completed plans only when investigating future work or design history.

If a statement conflicts, shipped code/config wins over current docs; current docs win over plans. Files under `Completed_Plans/` and `Project_Progress/` are intentionally historical and may contain values that were correct only at that stage.

## Current architecture

| Document | Covers |
|---|---|
| [`SOFTWARE_DESIGN.md`](SOFTWARE_DESIGN.md) | Hardware topology, firmware and edge responsibilities, packet table, command/reset behavior, build state |
| [`SOFTWARE_DESIGN_DIAGRAM.md`](SOFTWARE_DESIGN_DIAGRAM.md) | System, join, Timed-cycle, command, and framing diagrams |
| [`Current_Architecture/TUNABLE_PARAMETERS.md`](Current_Architecture/TUNABLE_PARAMETERS.md) | TDMA, reliability, sensing, power, base, and edge operating values |
| [`Current_Architecture/TDMA_PROTOCOL.md`](Current_Architecture/TDMA_PROTOCOL.md) | Slot geometry, assignment, clocks, TX budgets, RX gating, window markers |
| [`Current_Architecture/PACKET_RELIABILITY.md`](Current_Architecture/PACKET_RELIABILITY.md) | Link-ACK versus app-ACK modes, pending window, retry/ACK rules, sleeping nodes |
| [`Current_Architecture/DUTY_CYCLING.md`](Current_Architecture/DUTY_CYCLING.md) | Continuous, SensorTriggered, Timed, and Hybrid state/timing behavior |
| [`Current_Architecture/JETSON_BRIDGE.md`](Current_Architecture/JETSON_BRIDGE.md) | Native-USB framing, ingest, session startup, command and web routes |
| [`Current_Architecture/BANDWIDTH_SCALING.md`](Current_Architecture/BANDWIDTH_SCALING.md) | Bundle production, slot service, airtime estimates, scaling constraints |
| [`Current_Architecture/LORA_VS_LORAWAN.md`](Current_Architecture/LORA_VS_LORAWAN.md) | Raw-LoRa stack, LoRaWAN tradeoffs, collision scope, CAD, mesh, range levers |

## User references

| Document | Covers |
|---|---|
| [`User_Reference/FLASHING.md`](User_Reference/FLASHING.md) | Active PlatformIO targets, build/upload/monitor commands, troubleshooting |
| [`User_Reference/DEBUG_FILTER.md`](User_Reference/DEBUG_FILTER.md) | `@SFDBG` format and `SFDBG_*` monitor filters |
| [`User_Reference/JETSON_CHEATSHEET.md`](User_Reference/JETSON_CHEATSHEET.md) | Edge install/run commands, udev links, service/data operations |
| [`User_Reference/NETWORK_TEST.md`](User_Reference/NETWORK_TEST.md) | Current real-hardware LoRa -> base -> Jetson integration test |
| [`User_Reference/LORA_SNIFFER.md`](User_Reference/LORA_SNIFFER.md) | Passive sniffer firmware and dashboard integration |
| [`User_Reference/SMARTFIRES_MANAGER.md`](User_Reference/SMARTFIRES_MANAGER.md) | Jetson update/service/gateway-flashing manager |
| [`POWER_MEASURMENTS.md`](POWER_MEASURMENTS.md) | Isolated power-test environments and measurement setup |

## Pending plans

These contain remaining work. A plan may describe already-shipped prerequisites; its status section says what remains.

| Document | Current state at 2026-09-04 |
|---|---|
| [`Pending_Plans/BASE_SLOT_OVERRUN_FIX.md`](Pending_Plans/BASE_SLOT_OVERRUN_FIX.md) | Open: replace blocking base `ACK_SUMMARY` and direct `TIME_SYNC` paths with deadline-safe sending; command sending is already fire-and-forget |
| [`Pending_Plans/NATIVE_TEST_REPAIR.md`](Pending_Plans/NATIVE_TEST_REPAIR.md) | Open: repair removed duty-config factory references, controller assertions, and stale Arduino shim so `pio test -e native` is green |
| [`Pending_Plans/STANDBY_WATCHDOG_COVERAGE.md`](Pending_Plans/STANDBY_WATCHDOG_COVERAGE.md) | Open design choice: watchdog is disabled during Timed MCU standby |
| [`Pending_Plans/DYNAMIC_TX_POWER.md`](Pending_Plans/DYNAMIC_TX_POWER.md) | Control loop, packet, safeguards, and dashboard are shipped; field-tune constants and decide whether to raise the 13 dBm ceiling |
| [`Pending_Plans/RESET_REASON_DIAGNOSTICS.md`](Pending_Plans/RESET_REASON_DIAGNOSTICS.md) | Reset cause and hang-zone AWAKEN fields are shipped; hardware validation remains, optional early-warning PC capture is deferred |
| [`Pending_Plans/GPS_DISCIPLINED_CLOCK.md`](Pending_Plans/GPS_DISCIPLINED_CLOCK.md) | RTC COUNT32 timebase is shipped; GPS PPS discipline is not implemented |
| [`Pending_Plans/JETSON_SENSOR_EXPANSION.md`](Pending_Plans/JETSON_SENSOR_EXPANSION.md) | Direct Jetson temp/humidity, BMV080, GPS, and IMU expansion is not implemented; research spikes remain |

## Completed plans (historical)

| Document | Historical subject |
|---|---|
| [`Completed_Plans/BINARY_PACKET_PIPELINE.md`](Completed_Plans/BINARY_PACKET_PIPELINE.md) | Text-to-binary packet pipeline |
| [`Completed_Plans/BUNDLE_TIMESTAMP_FIX.md`](Completed_Plans/BUNDLE_TIMESTAMP_FIX.md) | Bundle timestamp correction |
| [`Completed_Plans/BOARD_REFACTOR_PLAN.md`](Completed_Plans/BOARD_REFACTOR_PLAN.md) | Firmware directory/class refactor |
| [`Completed_Plans/DEPLOYMENT_SCHEDULE.md`](Completed_Plans/DEPLOYMENT_SCHEDULE.md) | Heading/CLI deployment phases |
| [`Completed_Plans/DMP_CLEANUP_PLAN.md`](Completed_Plans/DMP_CLEANUP_PLAN.md) | ICM-20948 DMP heading design |
| [`Completed_Plans/EDGE_REFACTOR_WEB_DASHBOARD.md`](Completed_Plans/EDGE_REFACTOR_WEB_DASHBOARD.md) | Edge package/dashboard refactor |
| [`Completed_Plans/JETSON_CLI_AND_COMMAND_SYSTEM.md`](Completed_Plans/JETSON_CLI_AND_COMMAND_SYSTEM.md) | Earlier CLI/command design (not every proposed command shipped) |
| [`Completed_Plans/LINK_STATS_PACKET_PLAN.md`](Completed_Plans/LINK_STATS_PACKET_PLAN.md) | STATUS retransmit/failure counters |
| [`Completed_Plans/MCU_DUTY_CYCLE_CHANGELOG.md`](Completed_Plans/MCU_DUTY_CYCLE_CHANGELOG.md) | Review of the initial RTC standby change |
| [`Completed_Plans/NETWORK_RELIABILITY_NOTES.md`](Completed_Plans/NETWORK_RELIABILITY_NOTES.md) | Reliability investigation notes |
| [`Completed_Plans/ORIENTATION_CALIBRATION_PLAN.md`](Completed_Plans/ORIENTATION_CALIBRATION_PLAN.md) | Orientation/calibration concept |
| [`Completed_Plans/PERSISTENT_NODE_REGISTRY.md`](Completed_Plans/PERSISTENT_NODE_REGISTRY.md) | Jetson UID/node correlation persistence and patched AWAKEN forwarding |
| [`Completed_Plans/PHASE_PROGRESS.md`](Completed_Plans/PHASE_PROGRESS.md) | Staged reliability rollout |
| [`Completed_Plans/RADIO_RX_GATING.md`](Completed_Plans/RADIO_RX_GATING.md) | Node SX1276 receive-window gating |
| [`Completed_Plans/RESET_SYSTEM.md`](Completed_Plans/RESET_SYSTEM.md) | Base/node reset coordination |
| [`Completed_Plans/RTC_SUBSECOND_SLEEP.md`](Completed_Plans/RTC_SUBSECOND_SLEEP.md) | SAMD21 COUNT32 standby clock |
| [`Completed_Plans/TDMA_BUNDLE_SIZING.md`](Completed_Plans/TDMA_BUNDLE_SIZING.md) | Early airtime and bundle sizing |
| [`Completed_Plans/TDMA_SNIFFER_VISUALIZATION.md`](Completed_Plans/TDMA_SNIFFER_VISUALIZATION.md) | Sniffer timing dashboard |
| [`Completed_Plans/TELEMETRY_REWORK_PLAN.md`](Completed_Plans/TELEMETRY_REWORK_PLAN.md) | Original telemetry rework |
| [`Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md`](Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md) | Config-header consolidation |
| [`Completed_Plans/WATCHDOG_TIMER.md`](Completed_Plans/WATCHDOG_TIMER.md) | Node/base watchdog rollout |
| [`Completed_Plans/WINDOW_MARKER_PACKETS.md`](Completed_Plans/WINDOW_MARKER_PACKETS.md) | Dedicated Timed-window markers |

[`Project_Progress/Network_System_Design.md`](Project_Progress/Network_System_Design.md) is a historical narrative spanning several of these stages.

## Documentation maintenance

- [`DOC_FRONTMATTER.md`](DOC_FRONTMATTER.md) defines documentation metadata and `source_refs`.
- [`CODE_FRONTMATTER.md`](CODE_FRONTMATTER.md) defines reciprocal firmware file headers.
- `python3 documentation/check_doc_freshness.py` checks missing metadata, source paths, and stale verification dates.
- `python3 documentation/check_code_headers.py` checks C++ headers and doc/source backlink symmetry.

Run both checks after changing current documentation or firmware. A green metadata check proves structural consistency, not the truth of prose; review claims against code before changing `last_verified`.
