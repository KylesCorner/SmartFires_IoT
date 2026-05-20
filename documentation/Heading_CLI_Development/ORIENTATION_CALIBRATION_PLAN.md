# Node Orientation Calibration and Absolute Heading Plan

## Overview
This document outlines the plan for integrating IMU-based heading/orientation data into the SmartFires system, including calibration, data transmission, and persistent storage of calibration parameters.

## Goals
- Obtain absolute orientation (heading) for each node using IMU (magnetometer + accelerometer/gyro).
- Calibrate each node's IMU to correct for local magnetic distortions and tilt.
- Transmit calibration data from each node to the Jetson for persistent storage.
- Store calibration data on the Jetson as a dictionary mapping Serial Number (SN) to calibration parameters.
- Use GPS data to further refine/compensate heading (e.g., declination, movement-based heading).
- Transmit orientation data in status packets.
- Minimize the need for repeated calibration by reusing stored calibration data.

## High-Level Steps

1. **IMU Calibration on Node**
   - Implement a calibration routine on each node to determine hard/soft iron offsets and tilt compensation.
   - Trigger calibration via a `CALIBRATE` command from Jetson CLI.
   - Node performs calibration routine (move in figure-8 pattern, etc.) and computes calibration parameters.
   - Node stores calibration parameters in temporary memory during calibration session.

2. **Calibration Data Transmission to Jetson**
   - After calibration, node sends calibration parameters to Jetson via a dedicated calibration packet.
   - Include node SN in the packet for identification.
   - Jetson receives and stores in persistent dictionary: `{SN: calibration_params}`.

3. **Calibration Data Storage on Jetson**
   - Jetson maintains a persistent calibration dictionary loaded at startup: `{SN: calibration_params}`.
   - Provide CLI commands to view, update, or recalibrate nodes as needed.
   - On node reset or reconnection, calibration data remains on Jetson (not synced to node unless needed).

4. **Wake-Up Handshake: Send Calibration to Node**
   - When a node connects (wake-up handshake), Jetson sends the stored calibration data to the node.
   - Node stores calibration parameters in RAM for the current session.
   - Node can then use calibration data if needed for local processing, or simply send raw data.

5. **Raw IMU Data Transmission**
   - Node sends raw IMU data (magnetometer, accelerometer, gyro) in status packets to Jetson.
   - Include node SN for identification.

6. **Absolute Orientation Calculation on Jetson**
   - Jetson receives raw IMU data and applies stored calibration parameters.
   - Compute absolute heading (yaw), compensating for tilt.
   - Optionally use GPS data for further correction (e.g., magnetic declination, course over ground).
   - Store computed orientation per node for downstream analysis/control.

## Key Design Decisions
- **Calibration storage:** Persistent on Jetson only (not replicated to node).
- **Calibration computation:** On Jetson, using raw IMU data from nodes.
- **Calibration distribution:** Jetson sends calibration data to node during wake-up handshake (for reference, not required for operation).
- **Command mechanism:** Jetson CLI for sending calibrate, reset, and other commands to nodes.

## Open Questions
- What physical action/UI should trigger the calibration routine on a node? (e.g., button press, auto-trigger, manual command)
- What are the exact calibration parameters to store? (hard iron offsets, soft iron matrix, temperature compensation, etc.)
- Should nodes apply calibration locally or always send raw data? (Currently: send raw data, compute on Jetson)
- How to handle recalibration if a node's calibration drifts or becomes invalid?

## Next Steps
1. Define calibration parameter structure and packet format.
2. Design calibration trigger mechanism (CLI command, button, etc.).
3. Implement calibration routine on node (collect data, compute offsets/matrix).
4. Implement calibration packet transmission from node to Jetson.
5. Implement Jetson CLI for calibration commands (calibrate, store, load, view).
6. Implement calibration data storage and persistence on Jetson (JSON/pickle dictionary).
7. Implement wake-up handshake mechanism to send calibration data to node.
8. Implement raw IMU data transmission in status packets.
9. Implement orientation calculation on Jetson (apply calibration, compute heading with tilt/GPS correction).
10. Test end-to-end flow: calibrate node → store on Jetson → node reset → handshake sends calibration → raw data sent → orientation computed.

---

*This plan will be updated as implementation details are clarified.*
