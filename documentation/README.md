---
name: documentation-index
description: Table of contents for documentation/ — start here.
category: index
status: current
last_verified: 2026-08-21
related_docs:
  - software-design
---

# SmartFires IoT Documentation

## Top-level docs

- [SOFTWARE_DESIGN.md](SOFTWARE_DESIGN.md) — master system architecture: topology, runtime, data flow, wire protocol, build environments
- [SOFTWARE_DESIGN_DIAGRAM.md](SOFTWARE_DESIGN_DIAGRAM.md) — mermaid system diagram

## Current Architecture

In-depth reference for subsystems as they exist in the current codebase.

| Document | Covers |
|---|---|
| [TUNABLE_PARAMETERS.md](Current_Architecture/TUNABLE_PARAMETERS.md) | Every tunable constant — TDMA timing, sensor floors, duty cycle, battery, Jetson edge runtime |
| [TDMA_PROTOCOL.md](Current_Architecture/TDMA_PROTOCOL.md) | Slot assignment, frame layout, session clock, TX budget, Rx power gating, TIME_SYNC, boot handshake, scaling |
| [PACKET_RELIABILITY.md](Current_Architecture/PACKET_RELIABILITY.md) | StrictLinkAck vs AppLayerAckSummary modes, pending window, ACK_SUMMARY format and dispatch |
| [DUTY_CYCLING.md](Current_Architecture/DUTY_CYCLING.md) | DutyCycleController phases, config presets, trigger sensor, sample dispatch, error handling |
| [JETSON_BRIDGE.md](Current_Architecture/JETSON_BRIDGE.md) | Frame format, Feather↔Jetson protocol, FrameReceiver state machine, ingest loop, SessionManager |
| [BANDWIDTH_SCALING.md](Current_Architecture/BANDWIDTH_SCALING.md) | Airtime math, per-node service rate, node-count scaling table |
| [LORA_VS_LORAWAN.md](Current_Architecture/LORA_VS_LORAWAN.md) | Custom RadioHead/TDMA stack vs. LoRaWAN, mesh capability, CAD explainer, range/optimization levers |

## User Reference

Practical how-to guides for day-to-day development and deployment.

| Document | Covers |
|---|---|
| [FLASHING.md](User_Reference/FLASHING.md) | PlatformIO CLI commands to flash node and base firmware |
| [DEBUG_FILTER.md](User_Reference/DEBUG_FILTER.md) | Debug build structured logging and PlatformIO monitor filter |
| [JETSON_CHEATSHEET.md](User_Reference/JETSON_CHEATSHEET.md) | Jetson one-time setup, udev rules, edge-receiver install and run |
| [NETWORK_TEST.md](User_Reference/NETWORK_TEST.md) | End-to-end LoRa → base → Jetson integration test procedure |

## Pending Plans

Active design documents for work not yet implemented. Code is not yet authoritative for
these — check status tables within each doc for what's done vs. open.

Last audited 2026-08-21.

| Document | What it covers | State |
|---|---|---|
| [BASE_SLOT_OVERRUN_FIX.md](Pending_Plans/BASE_SLOT_OVERRUN_FIX.md) | Removes the last blocking `sendToWait()` from the base's slot-0 paths, adds a deadline-aware TX gate so no base send can start unless it fits the slot remainder, and repeats `PKT_WINDOW_END` | Not started. One open decision blocks it (`sendDirectTimeSync()`) |
| [NATIVE_TEST_REPAIR.md](Pending_Plans/NATIVE_TEST_REPAIR.md) | Everything keeping `pio test -e native` red: `test_config`'s removed duty-cycle factories, six `test_duty_cycle_controller` assertion failures, the stale `test/support/Arduino.cpp` shim | Not started |
| [STANDBY_WATCHDOG_COVERAGE.md](Pending_Plans/STANDBY_WATCHDOG_COVERAGE.md) | The watchdog is disabled across every MCU standby, leaving ~47% of wall-clock time with no hang recovery on a Timed node | Not started. Needs a decision between four options |
| [DYNAMIC_TX_POWER.md](Pending_Plans/DYNAMIC_TX_POWER.md) | Base-owned per-node TX power control loop (SNR margin + retry inhibitor), DYNAMIC/STATIC operator override, both link-failure fail-safes; dynamic SF deferred | **Shipped and flashed**; every constant still untuned, base ceiling raise not done |
| [RESET_REASON_DIAGNOSTICS.md](Pending_Plans/RESET_REASON_DIAGNOSTICS.md) | Reporting node reset cause + hang-zone breadcrumb through AWAKEN to attribute watchdog reboots (I2C stall vs RadioHead hang) | Phases 1–2 **shipped**; hardware validation and four wire-table doc updates outstanding |
| [GPS_DISCIPLINED_CLOCK.md](Pending_Plans/GPS_DISCIPLINED_CLOCK.md) | Run each node's clock continuously off RTC COUNT32 (Step 1, no GPS), then discipline its tick rate with the GPS 1 Hz PPS edge (Step 2) | Step 1 **shipped and baked**; Step 2 not started |
| [JETSON_SENSOR_EXPANSION.md](Pending_Plans/JETSON_SENSOR_EXPANSION.md) | Adding temp/humidity, BMV080, GPS, and ICM-20948 IMU sensors directly to the Jetson via I2C | Not started; two research spikes open (BMV080 SDK, GPS I2C→PTY bridge) |

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
| [RADIO_RX_GATING.md](Completed_Plans/RADIO_RX_GATING.md) | Sleeping the node's SX1276 outside the base's TDMA window to cut radio power draw |
| [WATCHDOG_TIMER.md](Completed_Plans/WATCHDOG_TIMER.md) | Hardware watchdog for nodes (Phase 1) and the base station (Phase 2) to auto-recover from unrecoverable hangs |
| [RESET_SYSTEM.md](Completed_Plans/RESET_SYSTEM.md) | Jetson/base/node reset coordination and time-sync recovery — base self-reset on `node_id=0`, node `CMD_RESET` execution, `TdmaClock::reset()`, Jetson session-start base reset |
| [MCU_DUTY_CYCLE_CHANGELOG.md](Completed_Plans/MCU_DUTY_CYCLE_CHANGELOG.md) | Review of commit `d7ba3c5` (SAMD21 RTC standby, Timed/Hybrid modes). Historical — four of its five implications drove the work below; **do not read its changelog as current behaviour** |
| [RTC_SUBSECOND_SLEEP.md](Completed_Plans/RTC_SUBSECOND_SLEEP.md) | Replacing RTCZero's whole-second calendar alarm with SAMD21 RTC COUNT32 (~1 ms), so a node resumes TDMA sync across MCU standby instead of re-handshaking every wake |
| [WINDOW_MARKER_PACKETS.md](Completed_Plans/WINDOW_MARKER_PACKETS.md) | Dedicated `PKT_WINDOW_BEGIN`/`PKT_WINDOW_END` frames replacing the window header flags, plus the fixed-period whole-bundle Timed window — removed the per-cycle duplicate bundle |
