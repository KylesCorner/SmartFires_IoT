---
name: deployment-schedule
description: Phased rollout schedule for the Jetson-side IMU calibration and absolute heading system, from wire protocol through interactive CLI and hardware validation.
category: plan-completed
status: historical
related_docs:
  - orientation-calibration-plan
  - jetson-cli-and-command-system
  - dmp-cleanup-plan
---

# Phased Deployment Schedule: Jetson CLI, Calibration, and Heading System

## Overview

This document sequences the work to add calibration-based absolute heading to SmartFires.

## Current Status Snapshot (2026-05-21)

- [x] Phase 0 implementation complete (protocol definitions and Python mirror)
- [x] Phase 1 implementation complete (base routing for new packet types)
- [x] Phase 2 implementation complete (session manager, AWAKEN mapping, calibration compute)
- [x] Phase 3 implementation complete (interactive CLI)
- [x] Phase 4 implementation complete (node calibration flow and command ACK path)
- [x] Phase 5 implementation complete (raw IMU in STATUS + heading display/log schema)
- [ ] Phase 6 hardware integration testing in progress
- [ ] Phase 7 optimization/polish pending

Phase 0-5 code is implemented and committed. The next gate is hardware validation across
the Phase 6 scenarios below.

Key architecture decisions reflected throughout:

- **Jetson computes everything.** Calibration fitting (full ellipsoid via eigendecomposition)
  and heading (tilt-compensated, declination-corrected) run entirely on the Jetson. The node
  does no IMU math beyond reading the sensor and accumulating running statistics.
- **Node sends a single calibration summary packet.** During the 60-second window the node
  computes mean, covariance, min, and max of magnetometer readings using the Welford online
  algorithm (O(N), constant memory). One 72-byte packet is transmitted at the end.
- **Raw IMU in STATUS packets.** Every 15-minute STATUS carries raw mag (x,y,z) and
  accel (x,y,z) as int16 (12 extra bytes). Jetson applies stored calibration on receipt.
- **uid_hash is the node identifier.** 32-bit FNV-1a of the SAMD21 128-bit serial;
  already in every AWAKEN payload. No protocol redesign needed.
- **No CALIBRATION_PUSH.** Calibration lives on the Jetson and is applied server-side.
  The node receives no calibration data back.
- **Existing AWAKEN flow is unchanged.** Jetson just needs to parse uid_hash from
  already-forwarded AWAKEN packets to build its uid_hash <-> node_id mapping.

---

## Phase 0: Wire Protocol and Packet Type Definitions (Week 1)

**Goal:** All new structs, constants, and encode/decode functions defined in firmware and
Python. No radio transmission of new types yet — just get definitions in place with
verified sizes.

### Firmware (node + base)

- [ ] Add new pkt_type constants to `BinaryPacket.h`:
  - `PKT_CMD_CALIBRATE    = 0x10`
  - `PKT_CMD_RESET        = 0x11`
  - `PKT_CALIBRATION_DATA = 0x12`
  - `PKT_CMD_ACK          = 0x13`
- [ ] Add packed structs to `BinaryPacket.h`:
  - `CmdCalibratePayload  { node_id:u8, duration_s:u8 }` — 2 bytes
  - `CmdResetPayload      { node_id:u8, reset_type:u8 }` — 2 bytes
  - `CalibrationDataPayload { uid_hash:u32, sample_count:u16, mag_mean[3]:f32, mag_cov[6]:f32, mag_min[3]:f32, mag_max[3]:f32, status:u8 }` — 68 bytes
  - `CmdAckPayload        { cmd_type:u8, uid_hash:u32, status:u8 }` — 6 bytes
- [ ] Add encode/decode functions for each new type (matching existing style)
- [ ] Extend `StatusPayload` from 12 to 24 bytes:
  - Add `mag_x:i16`, `mag_y:i16`, `mag_z:i16` (uT x 10)
  - Add `accel_x:i16`, `accel_y:i16`, `accel_z:i16` (mg)
  - Add `STATUS_IMU_VALID = 0x04` flag constant
- [ ] Update `static_assert` size checks for all modified and new structs
- [ ] Update `SensorSnapshot` with raw IMU fields:
  - `magX:i16`, `magY:i16`, `magZ:i16`, `accelX:i16`, `accelY:i16`, `accelZ:i16`, `imuValid:bool`
- [ ] Extend `pktTypeName()` in base app to cover all new types

### Jetson (Python)

- [ ] Add new constants to `packet.py` (PKT_CMD_CALIBRATE through PKT_CMD_ACK)
- [ ] Add struct formats and `decode_*` / `encode_*` functions for all new types
- [ ] Update `STATUS_PAYLOAD_FMT` and `STATUS_PAYLOAD_SIZE` for 24-byte payload
- [ ] Add `STATUS_IMU_VALID` constant
- [ ] Update `decode_status()` to extract and return mag and accel fields

### Validation

- [ ] All `static_assert` size checks pass in firmware build ✓
- [ ] `struct.calcsize()` for each new Python format matches `sizeof()` in C++ ✓
- [ ] `decode_status()` correctly parses 24-byte STATUS payload with IMU fields ✓
- [ ] Round-trip test: encode then decode each new packet type, verify field values ✓

---

## Phase 1: Base Station Routing for New Packet Types (Week 1-2)

**Goal:** Base station receives new Jetson command packets and forwards them to the correct
node. Base station receives new node packets and forwards them to the Jetson. Testable
without any node-side calibration logic.

### Firmware (base station — `SmartFiresBaseApp`)

- [ ] Extend `handleJetsonCommandPayload()`:
  - `PKT_CMD_CALIBRATE (0x10)`: decode `CmdCalibratePayload`, extract `node_id`,
    call `_radio.sendToWait(payload, len, node_id)`
  - `PKT_CMD_RESET (0x11)`: same pattern
- [ ] Extend `processIncomingLoRa()` forward-to-Jetson routing:
  - `PKT_CALIBRATION_DATA (0x12)`: forward via `encodeBaseFrame` (same as BUNDLE/STATUS)
  - `PKT_CMD_ACK (0x13)`: forward via `encodeBaseFrame`
  - Update packet type counter logging for new types

### Jetson (Python)

- [ ] Add UART frame encode functions: `encode_cmd_calibrate_frame()`, `encode_cmd_reset_frame()`
- [ ] Add `PKT_CALIBRATION_DATA` and `PKT_CMD_ACK` to `ingest_service.py` packet routing

### Validation

- [ ] Serial loopback: Jetson sends CMD_CALIBRATE frame → base logs correct node_id and
  `sendToWait` call ✓
- [ ] Hand-crafted CALIBRATION_DATA LoRa packet → base forwards to Jetson UART ✓
- [ ] Hand-crafted CMD_ACK LoRa packet → base forwards to Jetson UART ✓

---

## Phase 2: Jetson Session Management and AWAKEN Parsing (Week 2)

**Goal:** Jetson builds and persists the uid_hash <-> node_id mapping from AWAKEN packets.
SessionManager class handles all shared state. No calibration computation yet.

### Jetson (Python)

- [ ] Parse `AwakenPayload.uid_hash` from forwarded PKT_AWAKEN in `ingest_service.py`
- [ ] Create `SessionManager` class (new module `session.py`):
  - Load/save `~/.smartfires/session.json` with `threading.Lock`
  - `on_awaken(node_id, uid_hash)` → update uid_hash <-> node_id maps; log connection
    status ("calibration on file" vs "no calibration — run calibrate node N")
  - `on_calibration_data(node_id, uid_hash, stats)` → compute and store calibration
  - `on_cmd_ack(node_id, uid_hash, cmd_type, status)` → update command_queue
  - `on_status(node_id, uid_hash, status_dict)` → compute heading if calibration exists;
    update node_status
- [ ] Wire all handlers into `ingest_service.py`
- [ ] Implement calibration computation in `on_calibration_data()`:
  - Reconstruct 3x3 covariance from upper triangle
  - `eigenvalues, V = np.linalg.eigh(C)`
  - `soft_iron = V @ np.diag(1/sqrt(eigenvalues)) @ V.T`
  - `hard_iron = mag_mean`
  - Run quality checks (sample count, axis range, eigenvalue positivity)
  - Save to session.json
- [ ] Implement heading computation in `on_status()`:
  - Apply hard iron + soft iron correction
  - Compute roll/pitch from accel
  - Tilt-compensate magnetometer
  - Apply WMM magnetic declination from GPS lat/lon
  - Store in node_status; include in CSV/JSONL log rows

### Validation

- [ ] Node boots → Jetson parses uid_hash from AWAKEN → mapping logged and saved ✓
- [ ] Session survives Jetson restart (uid_hash mapping reloaded from session.json) ✓
- [ ] Manually inject a CALIBRATION_DATA UART frame → calibration computed and stored ✓
- [ ] Manually inject a STATUS frame with IMU fields → heading computed and logged ✓

---

## Phase 3: Jetson CLI Infrastructure (Week 3)

**Goal:** Interactive split-screen CLI with live packet log, command parsing and transmission,
ACK tracking, and heading display. Uses the SessionManager from Phase 2.

### Jetson (Python)

- [ ] Implement threaded architecture in new `cli.py` entry point:
  - **Listener Thread**: reads UART, parses packets, posts events to `queue.Queue`,
    calls SessionManager handlers
  - **UI Thread**: `curses` split screen — top 80% scrolling log, bottom command prompt
  - **Command Thread**: readline input, parses commands, sends UART frames
  - SessionManager passed to all threads; all state access under lock
- [ ] Implement command parser:
  - `calibrate node <id>` / `cal <id>` → `encode_cmd_calibrate_frame()` + queue entry
  - `reset node <id>` / `reset node <id> hard` → `encode_cmd_reset_frame()`
  - `list nodes` → tabular: node_id, uid_hash, last_seen, calib status, heading
  - `list calibrations` → uid_hash, sample_count, timestamp, eigenvalues
  - `save session` / `load session` / `clear calibration <id>` / `clear calibrations`
  - `help [command]`
- [ ] ACK timeout: warn after 5 s with no CMD_ACK
- [ ] CALIBRATION_DATA timeout: warn after `(duration_s + 15)` s with no data after ACK
- [ ] Graceful shutdown: save session, close serial

### Validation

- [ ] CLI starts, packet log updates in real-time with live nodes ✓
- [ ] `list nodes` shows uid_hash, last_seen, heading for any STATUS received ✓
- [ ] `calibrate node 1` sends UART frame → visible on base station debug serial ✓
- [ ] No ACK → warning displayed after 5 s ✓
- [ ] `save session` / `load session` round-trip preserves all data ✓

---

## Phase 4: Node Calibration Mode (Week 4)

**Goal:** Node receives CMD_CALIBRATE, collects IMU data for 60 seconds, computes the
statistical summary on-device using Welford's algorithm, and uploads CALIBRATION_DATA.

### Firmware (node)

- [ ] Add `CalibrationManager` class (or integrate into `SmartFiresNodeApp`):
  - State machine: `IDLE` / `CALIBRATING` / `UPLOADING`
  - On CMD_CALIBRATE received:
    - Verify `node_id` field matches own `NODE_ID`
    - Send CMD_ACK (status=processing)
    - Enter CALIBRATING state
  - In CALIBRATING state (called from `update()` loop):
    - Sample ICM-20948 magnetometer at ~10 Hz via `Icm20948Sensor`
    - Accumulate Welford running statistics:
      `n`, `mean[3]`, cross-product sums for upper-triangle covariance `M2[6]`,
      `min[3]`, `max[3]`
    - After `duration_s` seconds, finalize covariance: `cov[i] = M2[i] / (n-1)`
    - Encode `CalibrationDataPayload`
    - Enqueue for transmission (direct send outside TDMA slot is acceptable for
      a one-shot 72-byte packet)
    - Return to IDLE
- [ ] Implement CMD_CALIBRATE reception in `TdmaRadioService` or node receive path
- [ ] Implement CMD_ACK packet encoding and transmission helper
- [ ] Suspend BUNDLE transmission while CALIBRATING (set a flag checked by
  `SmartFiresNodeApp::update()` before `packetHandler.push()`)

### Validation (serial monitor on node)

- [ ] Node receives CMD_CALIBRATE → logs "entering calibration, duration=60s" ✓
- [ ] Node sends CMD_ACK → visible on base station debug UART ✓
- [ ] After 60 s: CALIBRATION_DATA transmitted → Jetson CLI shows calibration summary ✓
- [ ] Eigenvalues close to 1.0 with good rotation coverage ✓
- [ ] Node resumes BUNDLE transmission after calibration ✓

---

## Phase 5: Raw IMU in STATUS and Heading Display (Week 5)

**Goal:** `Icm20948Sensor::fillSnapshot()` populates raw IMU fields in `SensorSnapshot`.
`PacketHandler` encodes them into STATUS. Jetson displays heading in CLI.

### Firmware (node)

- [ ] Implement `Icm20948Sensor::fillSnapshot(SensorSnapshot&)`:
  - Read `_reading.magX/Y/Z` → convert to int16 (uT x 10) → `snap.magX/Y/Z`
  - Read `_reading.accelX/Y/Z` → convert to int16 (mg) → `snap.accelX/Y/Z`
  - Set `snap.imuValid = _reading.valid`
- [ ] Update `PacketHandler::tryEncodeStatus()`:
  - If `snap.imuValid`: set `STATUS_IMU_VALID` flag, populate `mag_x/y/z`, `accel_x/y/z`
- [ ] Confirm IMU bit (0x08) in `sensor_flags` is set when IMU data is valid

### Jetson (Python)

- [ ] Update `SessionManager.on_status()` to call heading computation when
  `STATUS_IMU_VALID` is set and calibration exists for the node's uid_hash
- [ ] Add heading, pitch, roll columns to telemetry CSV and status JSONL rows
- [ ] Update CLI `list nodes` to show heading column with degree symbol

### Validation

- [ ] Node with healthy ICM-20948 → STATUS packet has `STATUS_IMU_VALID` set ✓
- [ ] Node with no calibration → STATUS received, heading not computed, CLI shows `--` ✓
- [ ] Node with calibration → STATUS received → heading computed and logged ✓
- [ ] Rotate node physically → heading changes in expected direction on next STATUS ✓
- [ ] CLI `list nodes` shows heading for calibrated nodes ✓

---

## Phase 6: Integration Testing and End-to-End Validation (Week 6)

**Goal:** Full lifecycle test with at least two nodes.

### Test Scenarios

- [ ] **Cold start (no calibration):** Node boots → AWAKEN parsed → no calibration found →
  STATUS received with IMU data → heading not computed → CLI prompts user to calibrate ✓

- [ ] **Calibration flow:** `calibrate node 1` → CMD_CALIBRATE sent → CMD_ACK received →
  ~60 s BUNDLE gap → CALIBRATION_DATA received (72 bytes) → quality checks pass →
  calibration stored → next STATUS triggers heading computation ✓

- [ ] **Heading accuracy check:** Rotate calibrated node to known compass headings →
  verify Jetson-logged heading tracks correctly in direction and magnitude ✓

- [ ] **Multi-node:** Calibrate nodes 1 and 2 sequentially → both calibrations stored →
  both report heading in subsequent STATUS packets ✓

- [ ] **Reset command:** `reset node 2` → CMD_RESET → CMD_ACK → node reboots → AWAKEN →
  calibration still in session.json → next STATUS computes heading again ✓

- [ ] **Session persistence:** Restart Jetson process → session.json reloaded →
  next STATUS from any calibrated node immediately computes heading ✓

- [ ] **CLI responsiveness:** Send commands while BUNDLEs flowing → no packets dropped,
  no log entries missed, no command lag ✓

- [ ] **Quality rejection:** Calibrate with insufficient rotation (one axis stationary) →
  Jetson warns about axis range and/or rejects degenerate covariance ✓

### Phase 6 Execution Checkpoint

Use this table during execution and update in-place as scenarios complete.

| Scenario | Owner | Date | Result (PASS/FAIL) | Evidence Path | Notes |
| --- | --- | --- | --- | --- | --- |
| Cold start | | | | | |
| Calibration flow | | | | | |
| Heading accuracy check | | | | | |
| Multi-node | | | | | |
| Reset command | | | | | |
| Session persistence | | | | | |
| CLI responsiveness | | | | | |
| Quality rejection | | | | | |

---

## Phase 7: Optimization and Polish (Week 7)

- [ ] WMM magnetic declination lookup table (~2 KB ROM, Python dict) integrated into
  `on_status()` heading pipeline; verify true-north correction for deployment location
- [ ] Optional: simple IIR smoothing on Jetson-side heading output to reduce per-STATUS noise
- [ ] CLI colors: green = heading valid, yellow = no calibration, red = not seen > 60 s
- [ ] Log rotation and size limits for `jetson.log`
- [ ] CLI command history (readline) and tab completion
- [ ] 8-hour continuous run test: no memory leaks, no session corruption, no thread deadlock
- [ ] Documentation: CLI user guide, calibration procedure, troubleshooting guide

---

## Parallel Workstreams

| Workstream | Can overlap with |
| --- | --- |
| Phase 0 (protocol definitions) | Phase 1 (base routing) — define first, test together |
| Phase 2 (Jetson session + AWAKEN) | Phase 3 (CLI) — SessionManager used by both |
| Phase 4 (node calibration mode) | Phase 5 (raw IMU in STATUS) — both touch ICM-20948 driver |

Phases 0-1 are strictly prerequisite for everything else. After that, the Jetson track
(Phases 2-3) and firmware track (Phases 4-5) can proceed in parallel.

---

## Success Criteria by Phase

| Phase | Done When |
| --- | --- |
| 0 | All struct sizes verified; Python formats match C++; decode round-trips pass |
| 1 | Base station routes all 4 new packet types; verified via serial monitor |
| 2 | Jetson builds uid_hash <-> node_id map from AWAKEN; calibration computed from injected data; heading computed from injected STATUS |
| 3 | CLI running; commands sent; ACK tracked; live log visible; heading in list nodes |
| 4 | Node runs 60 s calibration; uploads CALIBRATION_DATA; Jetson stores calibration |
| 5 | STATUS packets carry valid IMU; Jetson computes and logs heading on receipt |
| 6 | Full lifecycle passes with 2+ nodes; accuracy spot-checked against known headings |
| 7 | 8-hour stable run; WMM declination applied; CLI polished and documented |

---

## Risk Register

| Risk | Mitigation |
| --- | --- |
| Welford covariance on SAMD21 runs out of memory | Algorithm uses O(1) memory (9 running sums for upper triangle); no issue at 48 KB SRAM |
| CALIBRATION_DATA 72-byte packet exceeds LoRa budget | 72 bytes well within 255-byte RadioHead limit; no issue |
| StatusPayload size change (12->24 bytes) breaks existing Jetson parser | Phase 0 updates `packet.py` first; `STATUS_PAYLOAD_SIZE` constant change triggers all callers; validated before hardware test |
| Covariance matrix degenerate if node not rotated enough | Jetson quality checks reject it with clear explanation; user must recalibrate with proper rotation |
| sendToWait for CMD_CALIBRATE fails (node not listening) | Node is always listening between TDMA TX slots; sendToWait retries once with 100 ms timeout |
| Heading noise on a single STATUS sample | Single sample accuracy is +-4-5 deg; acceptable for stationary deployed nodes. Optional Jetson-side IIR smoothing in Phase 7 |

---

## Timeline Summary

```
Week 1:   Phase 0  Protocol definitions — firmware + Python
Week 1-2: Phase 1  Base station routing for new packet types
Week 2:   Phase 2  Jetson session management + AWAKEN parsing + heading computation
Week 3:   Phase 3  Jetson CLI infrastructure
Week 4:   Phase 4  Node calibration mode (Welford statistics + CALIBRATION_DATA upload)
Week 5:   Phase 5  Raw IMU in STATUS + heading display in CLI
Week 6:   Phase 6  Integration testing with 2+ nodes
Week 7:   Phase 7  WMM declination, smoothing, polish, documentation

Total: ~7 weeks
```

Phases 2-3 (Jetson Python) and Phases 4-5 (firmware) can run in parallel once Phases 0-1
are complete, reducing calendar time to ~5-6 weeks with two concurrent workstreams.
