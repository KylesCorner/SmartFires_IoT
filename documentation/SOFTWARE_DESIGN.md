---
name: software-design
description: Master system architecture — hardware topology, firmware/edge module layout, wire protocol.
category: architecture
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/platformio.ini
  - platformio/include/config/NetworkProfiles.h
  - platformio/include/config/NetworkConfig.h
  - platformio/include/telemetry/BinaryPacket.h
  - platformio/src/app/SmartFiresBaseApp.cpp
  - edge/edge-receiver/src/smartfires_edge/main.py
  - edge/edge-receiver/src/smartfires_edge/network_profile.py
  - edge/edge-receiver/src/smartfires_edge/session_meta.py
related_docs:
  - software-design-diagram
  - tdma-protocol
  - jetson-bridge
---

# SmartFires IoT software design

## Scope and authority

This document describes the system that the current source tree builds. `platformio/platformio.ini`, firmware config headers, `BinaryPacket.h`, and the Python edge package remain authoritative for exact values. Possible plans preserve deferred ideas; completed plans preserve design history.

## System topology

```text
One or more Feather M0 RFM95 sensor nodes
  SHT31 temperature/humidity
  Modern Device Rev C wind sensor
  PA1010D GPS
  SPS30 particulate sensor
  ICM-20948 IMU/DMP
          |
          | 915 MHz raw LoRa, custom binary protocol, TDMA
          v
Feather M0 RFM95 base station
          |
          | native USB CDC (`Serial`), 115200 baud
          v
Jetson Orin Nano / `smartfires-edge`
```

Every remote unit has one Feather with sensors attached directly. `Serial1` belongs to the node's SPS30 connection. The base uses native USB CDC for the Jetson link; it does not use a hardware UART for that bridge.

## Firmware composition

`platformio/src/main.cpp` selects a build role and constructs its dependencies.

The node path is:

```text
sensor drivers -> sensor adapters -> DutyCycleController
               -> SensorSnapshot -> PacketHandler
               -> TdmaRadioService -> RadioHeadTdmaDriver
```

`SmartFiresNodeApp` waits for assignment/time sync, advances duty cycling, builds snapshots, queues bundle/status/window packets, receives commands, and manages sleep recovery. Sensor adapters populate `SensorSnapshot`; they do not know the wire format.

The base path is:

```text
RadioHeadTdmaDriver -> SmartFiresBaseApp -> native USB CDC
                         |       |
                         |       +-> TX-power controller
                         +-> node assignment, TIME_SYNC, ACK_SUMMARY, commands
```

The base is more than an opaque bridge. It parses headers, handles `AWAKEN`, assigns IDs, tracks received sequences, generates `ACK_SUMMARY`, extracts SNR from every assigned-node frame, decodes STATUS for retry/failure and applied-power feedback, and forwards valid LoRa payloads to the Jetson.

## Identity, joining, and time

The base is address/node ID 1 and owns TDMA slot 0. A new node starts unassigned, derives a temporary identity from the SAMD21 UID, and repeatedly sends fire-and-forget `AWAKEN` at a selected-profile cadence with bounded jitter. The base maps the UID hash to a session-local runtime node ID beginning at 2 and replies with fire-and-forget direct `TIME_SYNC`; the node adopts that ID and begins normal sensing. If the response is lost the node asks again. The base reuses the mapping while it remains running, but assignment order can change after a base reset.

`SMARTFIRES_NETWORK_PROFILE` selects an internally consistent SF7, SF9, SF10, or SF12 build for every network Feather. All current profiles use `NUM_SLOTS=5`: one base slot plus four assignable node slots. Changing the selector requires rebuilding the base, every node, and the passive sniffer; mixed profiles are unsupported.

The Jetson creates a session ID and supplies session-relative time every 600 seconds by default. The base caches that authority and broadcasts `TIME_SYNC` every 50/65/110/125 seconds for SF7/SF9/SF10/SF12. If the Jetson has not supplied time, the base uses a local session clock. Nodes regard sync as stale after 22 minutes and return to permissive recovery behavior.

## Sensing and duty cycling

The active production target, `feather_m0_lora_node`, uses Continuous mode after a 10-second warmup. Its sample cadence is 750 ms at SF7/SF9, 1 second at SF10, and 4 seconds at SF12.

`feather_m0_lora_node_debug` (the default build) and `feather_m0_lora_node_timed` use Timed mode. SF7/SF9/SF10 collect 30 samples at one second as two 15-sample bundles; SF12 collects 16 samples at four seconds as two eight-sample bundles. Their wake-to-wake cycles are 75/80/90/150 seconds. `node_hybrid` remains available; SensorTriggered is still a compiled controller mode but is not the production target.

Timed cycles send `WINDOW_BEGIN` and `WINDOW_END`. The end marker carries the planned standby and sample count so the base can defer downlink acknowledgements while that node is asleep.

## Radio and reliability

The deployed link uses raw LoRa at 915 MHz with RadioHead, nominally 13 dBm. SF7/SF9/SF10 explicitly program 125 kHz and SF12 programs 250 kHz; every profile uses CR 4/5, an eight-symbol preamble, explicit headers, payload CRC, and its selected SF. SF12 also enables low-data-rate optimization. Slot width, guards, receive wake-ahead, cadence, and reliability timers move with the profile.

Current node targets use app-layer reliability. Telemetry goes over the RadioHead link without waiting for a remote link ACK. The node keeps up to eight pending telemetry frames, while the base sends cumulative `ACK_SUMMARY` bitmaps. A pending frame expires after 30/40/70/150 seconds or three total attempts. Scheduled `AWAKEN`, direct assignment sync, `ACK_SUMMARY`, time sync, and commands are fire-and-forget; repeated application state or `CMD_ACK` supplies recovery. Actual-length airtime admission prevents base and node scheduled sends from starting too near the trailing guard.

## Wire protocol

Every current LoRa packet begins with a five-byte header (`magic`, `type`, `node_id`, `seq`, `flags`) and ends with CRC-8/MAXIM. Sizes below include the CRC.

| Packet | Direction | Bytes | Purpose |
|---|---|---:|---|
| `FULL_STATE` | node -> base | 26 | One uncompressed sensor snapshot |
| `BUNDLE` | node -> base | up to 195 | One 20-byte reference plus up to fourteen 12-byte deltas |
| `STATUS` | node -> base | 27 | GPS, battery, heading, reliability totals, applied TX power/mode |
| `AWAKEN` | node -> base | 12 | UID hash, reset cause, hang zone; legacy 9-byte form is accepted |
| `ACK_SUMMARY` | base -> node | 10 | Cumulative base sequence plus 16-bit look-ahead mask |
| `WINDOW_BEGIN/END` | node -> base | 17 | Timed active-window lifecycle |
| `TIME_SYNC` | base -> node | 14 | Session ID and session-relative milliseconds |
| `CMD_CALIBRATE` | base -> node | 8 | Calibration request; currently log-and-ACK by design |
| `CMD_RESET` | base -> node | 8 | Soft rejoin or hard MCU reset |
| `CMD_SET_TX_POWER` | base -> node | 9 | Absolute dBm plus DYNAMIC/STATIC mode |
| `CMD_ACK` | node -> base | 12 | Application acknowledgement for a command |
| `DEBUG_LOG` | base -> Jetson only | variable | Structured `@SFDBG` text in the USB envelope |
| `NETWORK_PROFILE` | base -> Jetson only | 37-byte header + control payload | Selected radio/TDMA/cadence identity and fingerprint |

STATUS cadence is profile-derived: 15/30/120/300 seconds for SF7/SF9/SF10/SF12. Packet definitions must be changed in lockstep in C++ and `smartfires_edge/packet.py`.

## Base-to-Jetson bridge

Base-to-Jetson frames are:

```text
AA 55 | data_len | RSSI:i8 | LoRa packet bytes | CRC-8
```

Jetson-to-base frames omit RSSI:

```text
AA 55 | data_len | command/time-sync packet bytes | CRC-8
```

The outer CRC covers `data_len` and its data. Embedded LoRa packets retain their own CRC; USB-only `DEBUG_LOG` and `NETWORK_PROFILE` controls rely on the outer CRC.

## Edge software

The package under `edge/edge-receiver` exposes exactly four commands:

- `receive`: reconnecting serial ingest and durable CSV/JSONL state.
- `summary`: packet-loss summary from stored state.
- `visualize`: live terminal telemetry/status tables.
- `web`: ingest plus FastAPI dashboard and optional passive-sniffer feed.

The edge default is `/dev/smartfires-base`, 115200 baud, `/mnt/nvme_drive/data`, nodes 2/3/4, and web port 8080. Ingest sends a base soft-reset command when a serial session starts, then supplies a new time session. Profile state begins unknown, becomes base-authoritative on a valid five-second announcement, appears at `/api/network_profile` and in the dashboard header, drives sample/TDMA visualization, and is persisted atomically in `session.json`. A recovery JSON/CLI override remains explicit and disagreements are reported as mismatches. A profile change after telemetry begins is recorded but does not silently replace the session's pinned identity.

The dashboard's node-reset and TX-power endpoints are functional. `/api/command` is only an echo stub.

## Reset, calibration, and TX power

- Soft node reset clears sync and telemetry buffers, resets packet state, and immediately returns to `AWAKEN`.
- Hard node reset sends a nonblocking `CMD_ACK`, waits briefly, and calls `NVIC_SystemReset()`.
- A reset addressed to node 0 applies to the base. The Jetson uses that path when opening a fresh ingest session.
- Calibration commands are decoded and acknowledged but do not start a separate routine; DMP fusion self-calibrates.
- The base-owned TX-power controller uses profile-specific SNR floor plus STATUS retry/failure deltas. Operators can switch a node between DYNAMIC and STATIC or select an absolute 5–13 dBm level through `/api/tx_power`. SF7 defaults DYNAMIC; higher-SF profiles default STATIC 13 dBm for acceptance testing. Thresholds still need field tuning.

## Build environments and validation state

Network targets are `feather_m0_lora_base`, `feather_m0_lora_node`, `feather_m0_lora_node_debug`, `feather_m0_lora_node_timed`, `feather_m0_lora_node_hybrid`, and `feather_m0_lora_sniffer`. Isolated `feather_m0_power_*` targets cover power measurements. Removed dummy-node and sensor-probe targets are not valid commands.

`native` contains host Unity tests, including all four profile definitions and airtime invariants. The broader suite has known unrelated failures documented in `Possible_Plans/NATIVE_TEST_REPAIR.md`. Higher-SF modem timing, guards, delivery, energy, and range remain provisional until bench and field acceptance; no firmware build alone establishes hardware validity.
