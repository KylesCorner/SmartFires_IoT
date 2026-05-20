# Phased Deployment Schedule: Jetson CLI, Calibration, and Heading System

## Overview
This document outlines a phased approach to deploy the Jetson CLI/command system, node calibration workflow, and absolute heading calculation. Phases are sequenced to allow for testing and validation at each stage.

---

## Phase 0: Foundation (Week 1)
**Goal:** Establish packet types, data structures, and basic node discovery.

### Node-Side Tasks
- [ ] Define packet type constants (0x10=CALIBRATE, 0x11=RESET, 0x12=CALIBRATION_DATA, 0x13=ACK)
- [ ] Implement packet reception handler for CALIBRATE and RESET commands
- [ ] Add ACK packet transmission (acknowledge command receipt)
- [ ] Update HANDSHAKE packet to include node_id assignment from base station

### Jetson-Side Tasks
- [ ] Define packet parsing for all new packet types
- [ ] Create session state data structure (node_id↔SN mapping, calibrations dict, command queue)
- [ ] Implement JSON persistence layer (save/load session to disk)
- [ ] Test node discovery: parse HANDSHAKE and populate node_id↔SN mapping

### Base Station (Feather)
- [ ] Ensure forward routing of new packet types (0x10, 0x11, 0x12, 0x13) to Jetson

### Validation
- [ ] Node connects → Jetson receives HANDSHAKE → node_id↔SN mapping created ✓
- [ ] Jetson saves/loads session.json correctly ✓

---

## Phase 1: Jetson CLI & Command Infrastructure (Week 2)
**Goal:** Build interactive CLI with command parsing and transmission.

### Jetson-Side Tasks
- [ ] Implement threaded architecture:
  - [ ] Packet Listener Thread (reads from serial/LoRa, parses packets)
  - [ ] UI Thread (displays packet log + command prompt)
  - [ ] Command Handler Thread (parses user input, builds packets)
  - [ ] Session Manager (thread-safe access to shared state)
- [ ] Implement split-screen display (top=packet log, bottom=command input)
- [ ] Implement command parser:
  - [ ] `calibrate node <id>` → build CALIBRATE packet
  - [ ] `reset node <id>` → build RESET packet
  - [ ] `list nodes` → display node_id, SN, last_seen, calibration status
  - [ ] `help` → display available commands
- [ ] Implement command transmission (send packets to base station)
- [ ] Implement ACK reception and feedback display

### Testing
- [ ] Send `calibrate node 1` → verify packet reaches node ✓
- [ ] Node sends ACK → verify CLI displays "ACK received" ✓
- [ ] Multiple commands queued and tracked ✓
- [ ] CLI remains responsive to new packets while commands are pending ✓

---

## Phase 2: Node Calibration Mode & Data Flow (Week 3)
**Goal:** Implement calibration routine on node and data transmission back to Jetson.

### Node-Side Tasks
- [ ] Implement calibration mode state machine:
  - [ ] On CALIBRATE receipt: pause normal sampling, enter calibration state
  - [ ] Collect raw magnetometer/accelerometer data for 60 seconds
  - [ ] Compute hard iron offsets (mean centering)
  - [ ] Compute soft iron matrix (using method: e.g., ellipsoid fitting or known algorithm)
  - [ ] Package calibration data into CALIBRATION_DATA packet
  - [ ] Transmit CALIBRATION_DATA to base station
  - [ ] Resume normal sampling after calibration complete
- [ ] Test with oscilloscope/serial monitor: verify 60s collection, computation, transmission timing
- [ ] Handle calibration interruption (reset during calibration → abort gracefully)

### Jetson-Side Tasks
- [ ] Implement CALIBRATION_DATA reception and parsing
- [ ] Extract hard iron offsets and soft iron matrix from packet
- [ ] Store in calibrations dict: `calibrations[SN] = {hard_iron: [...], soft_iron: [...], timestamp: ...}`
- [ ] Update node_status: `node_status[node_id]["calibrating"] = False`
- [ ] Save session to disk after calibration received
- [ ] Display calibration data in CLI with formatted output

### Testing
- [ ] Send `calibrate node 1` from CLI
- [ ] Node enters calibration mode for 60s
- [ ] Node sends CALIBRATION_DATA
- [ ] Jetson receives, parses, stores to session.json ✓
- [ ] Verify calibration data persists across Jetson restart ✓
- [ ] `list calibrations` shows stored data ✓

---

## Phase 3: IMU Raw Data Integration (Week 4)
**Goal:** Add raw IMU data to status packets and establish Jetson-side storage.

### Node-Side Tasks
- [ ] Modify STATUS packet to include:
  - [ ] Raw magnetometer readings (x, y, z as int16 or float32)
  - [ ] Raw accelerometer readings (x, y, z)
  - [ ] Optional: gyroscope readings (x, y, z)
- [ ] Ensure STATUS packets still transmit at normal rate (don't add latency)
- [ ] Validate data range and sampling quality

### Jetson-Side Tasks
- [ ] Parse raw IMU data from STATUS packets
- [ ] Store per-node IMU buffer: `imu_data[sn] = [timestamp, mag_x, mag_y, mag_z, accel_x, ...]`
- [ ] Option: Stream to file or database for post-processing
- [ ] Display IMU values in CLI (optional, may slow UI)

### Testing
- [ ] Receive STATUS packets with IMU data ✓
- [ ] Verify data format and range ✓
- [ ] Store to disk for analysis ✓

---

## Phase 4: Absolute Heading Calculation (Week 5)
**Goal:** Compute and store absolute orientation (heading) on Jetson.

### Jetson-Side Tasks
- [ ] Implement heading calculation function:
  - [ ] Input: raw mag (x, y, z), raw accel (x, y, z), calibration params (hard iron, soft iron)
  - [ ] Output: heading (yaw in degrees, 0-360)
  - [ ] Apply hard iron offset: `mag_corrected = mag - hard_iron`
  - [ ] Apply soft iron matrix: `mag_corrected = soft_iron @ mag_corrected`
  - [ ] Calculate tilt correction (pitch, roll from accelerometer)
  - [ ] Calculate heading from tilt-corrected magnetometer
  - [ ] Optional: Apply magnetic declination from GPS latitude/longitude
- [ ] Store computed orientation per node: `orientation[sn] = {heading: ..., pitch: ..., roll: ..., timestamp: ...}`
- [ ] Validate heading range and smoothness

### Testing
- [ ] Receive raw IMU data from node with known calibration
- [ ] Manually verify heading calculation (e.g., rotate node, check output changes)
- [ ] Compare with expected values (e.g., known direction)

---

## Phase 5: Wake-Up Handshake & Calibration Distribution (Week 6)
**Goal:** Send stored calibration data to node on reconnection.

### Node-Side Tasks
- [ ] On wake-up/reconnection, listen for calibration data in handshake response or dedicated packet
- [ ] Store received calibration in RAM for current session
- [ ] (Optional: use calibration for local heading calculation instead of sending raw data)

### Jetson-Side Tasks
- [ ] On HANDSHAKE receipt:
  - [ ] Lookup SN in calibrations dict
  - [ ] If calibration exists, send CALIBRATION_DATA packet back to node
  - [ ] Log: `[17:30:20.456] Node 1 (SN: 0x1234) connected. Calibration data sent.`
- [ ] Implement timeout: if node doesn't request calibration within N seconds, resend or log warning

### Testing
- [ ] Node resets/reconnects → Jetson sends calibration data
- [ ] Node confirms receipt (via ACK or status message)
- [ ] Verify node now has calibration available for current session

---

## Phase 6: Status Packet Integration & Orientation Transmission (Week 7)
**Goal:** Include computed heading in status packets (optional, or keep raw data only).

### Option A: Jetson Computes Only (Recommended)
- [ ] Keep nodes sending raw IMU data
- [ ] Jetson computes and stores orientation
- [ ] No transmission of orientation back to nodes (simplifies node code)

### Option B: Node Computes & Transmits (Future)
- [ ] Node applies calibration locally, computes heading
- [ ] Node includes heading in STATUS packet
- [ ] Jetson validates/logs orientation data
- [ ] Reduces Jetson computation but increases node complexity

### Recommended for Phase 6: Finalize Option A
- [ ] No code changes needed; verify Jetson-side computation is accurate and continuous

---

## Phase 7: Integration Testing & End-to-End Validation (Week 8)
**Goal:** Full system test with multiple nodes.

### Test Scenarios
- [ ] **Cold Start:** Power on all nodes → handshakes → calibration data distributed → raw IMU received → headings computed ✓
- [ ] **Calibration Flow:** `calibrate node 1` → 60s collection → data received → stored → persisted ✓
- [ ] **Reset:** `reset node 2` → node reboots → reconnects → calibration data resent ✓
- [ ] **Multiple Nodes:** Calibrate nodes 1, 2, 3 in sequence → all calibrations stored and applied ✓
- [ ] **Data Accuracy:** Rotate node, verify heading changes correctly ✓
- [ ] **CLI Responsiveness:** Send commands while packets flowing → no lag or data loss ✓

### Field Testing (Optional)
- [ ] Deploy with GPS data to test magnetic declination correction
- [ ] Verify heading accuracy in real-world environment

---

## Phase 8: Optimization & Polish (Week 9)
**Goal:** Refine, optimize, and prepare for production.

### Tasks
- [ ] Optimize packet parsing and transmission latency
- [ ] Improve CLI display (better formatting, colors, logging)
- [ ] Add telemetry and performance metrics (packet loss, latency, compute time)
- [ ] Error handling and edge cases (node disconnects, malformed packets, etc.)
- [ ] Documentation: CLI user guide, calibration procedure, troubleshooting

---

## Parallel Workstreams

While some phases are sequential, the following can be parallelized:

- **Phase 0 & 1:** Can overlap — define packets in 0, implement CLI in 1
- **Phase 3 & 4:** Can overlap — add IMU to STATUS in 3, implement heading calculation in 4
- **Phase 2 & 3:** Must be sequential (calibration needed before using IMU data)

---

## Success Criteria by Phase

| Phase | Success Criteria |
|-------|-----------------|
| 0     | Node connects, SN→node_id mapping created, session persists |
| 1     | CLI commands sent, ACKs received, commands logged |
| 2     | Calibration routine executes, data transmitted and stored |
| 3     | Raw IMU data in STATUS packets, stored on Jetson |
| 4     | Heading computed from raw IMU + calibration, validated accurate |
| 5     | Calibration distributed on node reconnection |
| 6     | Heading computation continuous and accurate |
| 7     | Full system test passes with multiple nodes |
| 8     | Production-ready, documented, optimized |

---

## Risk Mitigation

- **Packet Loss:** Add retry logic and timeouts for critical commands (calibrate, reset).
- **Node Hang:** Implement watchdog timer to detect unresponsive nodes.
- **Calibration Failure:** Allow manual recalibration without re-flashing node.
- **Data Sync:** Session.json as single source of truth; backup before major changes.

---

## Timeline Summary

```
Week 1: Phase 0 (Foundation)
Week 2: Phase 1 (CLI Infrastructure)
Week 3: Phase 2 (Calibration)
Week 4: Phase 3 (IMU Raw Data)
Week 5: Phase 4 (Heading Calculation)
Week 6: Phase 5 (Handshake & Calibration Distribution)
Week 7: Phase 6 (Status Packet Integration)
Week 8: Phase 7 (Integration Testing)
Week 9: Phase 8 (Optimization & Polish)

Total: ~9 weeks
```

---

*This schedule is adaptive. Phases may overlap or be adjusted based on testing results and resource availability.*
