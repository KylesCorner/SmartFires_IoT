# SmartFires IoT Documentation

## Top-level docs

- [SOFTWARE_DESIGN.md](SOFTWARE_DESIGN.md) — master system architecture: topology, runtime, data flow, wire protocol, build environments
- [SOFTWARE_DESIGN_DIAGRAM.md](SOFTWARE_DESIGN_DIAGRAM.md) — mermaid system diagram

## Current Architecture

In-depth reference for subsystems as they exist in the current codebase.

| Document | Covers |
|---|---|
| [TUNABLE_PARAMETERS.md](Current_Architecture/TUNABLE_PARAMETERS.md) | Every tunable constant — TDMA timing, sensor floors, duty cycle, battery, Jetson edge runtime |
| [TDMA_PROTOCOL.md](Current_Architecture/TDMA_PROTOCOL.md) | Slot assignment, frame layout, session clock, TX budget, TIME_SYNC, boot handshake, scaling |
| [PACKET_RELIABILITY.md](Current_Architecture/PACKET_RELIABILITY.md) | StrictLinkAck vs AppLayerAckSummary modes, pending window, ACK_SUMMARY format and dispatch |
| [DUTY_CYCLING.md](Current_Architecture/DUTY_CYCLING.md) | DutyCycleController phases, config presets, trigger sensor, sample dispatch, error handling |
| [UART_JETSON_BRIDGE.md](Current_Architecture/UART_JETSON_BRIDGE.md) | Frame format, Feather↔Jetson protocol, FrameReceiver state machine, ingest loop, SessionManager |
| [BANDWIDTH_SCALING.md](Current_Architecture/BANDWIDTH_SCALING.md) | Airtime math, per-node service rate, node-count scaling table |

## User Reference

Practical how-to guides for day-to-day development and deployment.

| Document | Covers |
|---|---|
| [FLASHING.md](User_Reference/FLASHING.md) | PlatformIO CLI commands to flash node and base firmware |
| [DEBUG_FILTER.md](User_Reference/DEBUG_FILTER.md) | Debug build structured logging and PlatformIO monitor filter |
| [JETSON_CHEATSHEET.md](User_Reference/JETSON_CHEATSHEET.md) | Jetson one-time setup, UART enable, edge-receiver install and run |

## Completed Plans

Historical design and implementation documents. Accurate at time of writing;
the code is now the authoritative record.

| Document | What it captured |
|---|---|
| [BINARY_PACKET_PIPELINE.md](Completed_Plans/BINARY_PACKET_PIPELINE.md) | Binary protocol rollout plan and field layout reference |
| [PHASE_PROGRESS.md](Completed_Plans/PHASE_PROGRESS.md) | Staged reliability migration (Phases 1–5) |
| [NETWORK_RELIABILITY_NOTES.md](Completed_Plans/NETWORK_RELIABILITY_NOTES.md) | Pre-design reliability debugging session notes |
| [TDMA_BUNDLE_SIZING.md](Completed_Plans/TDMA_BUNDLE_SIZING.md) | Early TDMA slot and bundle sizing derivation |
| [TELEMETRY_REWORK_PLAN.md](Completed_Plans/TELEMETRY_REWORK_PLAN.md) | Original telemetry text→binary migration plan |
| [BOARD_REFACTOR_PLAN.md](Completed_Plans/BOARD_REFACTOR_PLAN.md) | Directory structure refactor plan |
| [DMP_CLEANUP_PLAN.md](Completed_Plans/DMP_CLEANUP_PLAN.md) | ICM-20948 DMP heading computation design |
| [DEPLOYMENT_SCHEDULE.md](Completed_Plans/DEPLOYMENT_SCHEDULE.md) | Phased heading/CLI deployment schedule (7 phases) |
| [JETSON_CLI_AND_COMMAND_SYSTEM.md](Completed_Plans/JETSON_CLI_AND_COMMAND_SYSTEM.md) | Jetson split-screen CLI and command system design |
| [ORIENTATION_CALIBRATION_PLAN.md](Completed_Plans/ORIENTATION_CALIBRATION_PLAN.md) | Node orientation calibration and absolute heading plan |
| [LINK_STATS_PACKET_PLAN.md](Completed_Plans/LINK_STATS_PACKET_PLAN.md) | Extending PKT_STATUS with lifetime retransmit/fail counters |
