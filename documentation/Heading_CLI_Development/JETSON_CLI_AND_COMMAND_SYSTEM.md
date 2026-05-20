# Jetson CLI and Command System Design

## Overview
This document details the design for a split-screen CLI on the Jetson that allows viewing live packet data while simultaneously sending commands (CALIBRATE, RESET) to nodes. The system maintains a persistent session state mapping node IDs to serial numbers, stores IMU calibration parameters, and enables graceful command handling with acknowledgments.

---

## Architecture

### High-Level Design
```
┌─────────────────────────────────────────────┐
│          Jetson Receiver Process             │
├─────────────────────────────────────────────┤
│  ┌──────────────────────────────────────┐   │
│  │   Packet Listener Thread             │   │
│  │  (LoRa/Serial Input)                 │   │
│  └────────────┬─────────────────────────┘   │
│               │                             │
│  ┌────────────▼──────────────────────────┐  │
│  │   Packet Parser & Router              │  │
│  │  - Parse incoming packets             │  │
│  │  - Route to handlers                  │  │
│  │  - Update session state               │  │
│  └────────────┬─────────────────────────┘  │
│               │                            │
│  ┌────────────▼──────────────────────────┐ │
│  │   UI/Display Thread                   │ │
│  │  - Show packet log (top half)         │ │
│  │  - Command input (bottom half)        │ │
│  └──────────────────────────────────────┘ │
│               ▲                            │
│               │                            │
│  ┌────────────┴──────────────────────────┐ │
│  │   Command Handler Thread              │ │
│  │  - Parse user input                   │ │
│  │  - Build command packets              │ │
│  │  - Send to base station               │ │
│  └──────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

### Threads/Concurrency
- **Listener Thread:** Continuously reads from serial/LoRa, parses packets, updates session state.
- **UI Thread:** Displays packet log and command prompt (non-blocking input).
- **Command Thread:** Handles user input parsing and command transmission.
- **Session Manager:** Thread-safe access to node mappings and calibration data.

---

## Packet Types

### Existing Packet Types
- STATUS: Regular sensor data from nodes (includes raw IMU if applicable).
- HANDSHAKE: Initial connection from node with SN, node_id assignment.

### New Packet Types

#### 1. CALIBRATE Command (Jetson → Node)
```
[Packet Type: 0x10]
[SN: 2 bytes] or [0xFFFF for ALL]
[Duration: 1 byte] (in 10-second units, typically 0x06 = 60 seconds)
```

**Flow:**
1. Jetson sends CALIBRATE packet to base station with target SN.
2. Node receives, acknowledges with ACK packet, enters calibration mode.
3. Node pauses normal sampling.
4. Node collects raw IMU data for 60 seconds.
5. Node computes calibration parameters (hard iron offsets, soft iron matrix).
6. Node sends CALIBRATION_DATA packet back to Jetson.

---

#### 2. RESET Command (Jetson → Node)
```
[Packet Type: 0x11]
[SN: 2 bytes] or [0xFFFF for ALL]
[Reset Type: 1 byte] (0x00 = soft reset, 0x01 = hard reset, etc.)
```

**Flow:**
1. Jetson sends RESET packet to base station with target SN.
2. Node receives, acknowledges with ACK packet.
3. Node performs reset action (reboot, clear state, etc.).

---

#### 3. CALIBRATION_DATA (Node → Jetson)
```
[Packet Type: 0x12]
[SN: 2 bytes]
[Hard Iron Offsets: 12 bytes] (3x float32: x, y, z)
[Soft Iron Matrix: 36 bytes] (9x float32: 3x3 matrix, row-major)
[Timestamp: 4 bytes] (Unix epoch or node timestamp)
[Status: 1 byte] (0x00 = success, 0x01 = incomplete, 0x02 = error)
```

**Note:** If soft iron matrix is diagonal-only, reduce to 12 bytes (3x float32). Adjust as needed.

---

#### 4. ACK (Node → Jetson)
```
[Packet Type: 0x13]
[Command Type: 1 byte] (0x10 = CALIBRATE, 0x11 = RESET, etc.)
[SN: 2 bytes]
[Status: 1 byte] (0x00 = received, 0x01 = processing, 0x02 = error)
[Message: Variable] (optional, e.g., error reason)
```

---

## Session State & Persistence

### In-Memory Session Structure
```python
{
    "node_id_to_sn": {
        1: 0x1234,       # node_id 1 → SN 0x1234
        2: 0x5678,
        3: 0x9ABC,
    },
    "sn_to_node_id": {
        0x1234: 1,       # reverse mapping for quick lookup
        0x5678: 2,
        0x9ABC: 3,
    },
    "calibrations": {
        0x1234: {
            "hard_iron": [x, y, z],
            "soft_iron": [[...], [...], [...]],  # 3x3 matrix
            "timestamp": 1234567890,
            "status": "valid"
        },
        0x5678: { ... },
    },
    "command_queue": [
        {"type": "CALIBRATE", "sn": 0x1234, "sent_at": ..., "acked": False},
        ...
    ],
    "node_status": {
        1: {"last_seen": ..., "calibrating": False, "state": "idle"},
        2: {...},
    }
}
```

### Persistence to Disk
**File: `~/.smartfires/session.json`** (or configurable path)
```json
{
    "node_id_to_sn": { ... },
    "calibrations": { ... },
    "last_updated": 1234567890
}
```

**On startup:**
- Load `session.json` to populate node→SN mapping and calibration data.
- Listening mode will update mappings if nodes report new IDs.

**On shutdown or command:**
- Save current session to disk.

---

## CLI Interface

### Layout (Split-Screen)
```
═══════════════════════════════════════════════════════════════════════════
│                         SMARTFIRES JETSON CLI                            │
═══════════════════════════════════════════════════════════════════════════
│                                                                           │
│  [PACKET LOG - Top 80% of terminal]                                     │
│                                                                           │
│  [17:30:01.234] STATUS from Node 1 (SN: 0x1234) - Temp: 25C, RH: 45%   │
│  [17:30:02.456] STATUS from Node 2 (SN: 0x5678) - Temp: 26C, RH: 43%   │
│  [17:30:03.101] ACK: CALIBRATE received by Node 1                        │
│  [17:30:05.234] STATUS from Node 3 (SN: 0x9ABC) - Temp: 24C, RH: 47%   │
│                                                                           │
│  ...                                                                     │
│                                                                           │
├───────────────────────────────────────────────────────────────────────────┤
│  COMMAND INPUT (Bottom 20% of terminal)                                  │
│  > calibrate node 1                                                      │
│  [Sent] CALIBRATE to Node 1 (SN: 0x1234). Waiting for ACK...            │
│  >                                                                        │
└───────────────────────────────────────────────────────────────────────────┘
```

### Command Syntax
All commands are simple and case-insensitive.

#### Calibrate a Node
```
calibrate node <node_id>
calibrate <node_id>
cal <node_id>
calibrate all
```

**Example:**
```
> calibrate node 1
[17:30:10.234] Sending CALIBRATE to Node 1 (SN: 0x1234)...
[17:30:10.450] ACK received: Node 1 acknowledged CALIBRATE. Calibrating for 60s...
[17:30:70.452] CALIBRATION_DATA received from Node 1:
  - Hard Iron: [1.2, -0.5, 0.8]
  - Soft Iron: [[0.98, 0.01, 0.02], ...]
  - Status: success
[17:30:70.500] Calibration data for SN 0x1234 saved to session.
```

#### Reset a Node
```
reset node <node_id>
reset <node_id>
```

**Example:**
```
> reset node 2
[17:30:15.234] Sending RESET to Node 2 (SN: 0x5678)...
[17:30:15.450] ACK received: Node 2 acknowledged RESET. Resetting...
[17:30:16.000] Node 2 reset complete (reconnected with new status).
```

#### List Nodes & Calibrations
```
list nodes
list calibrations
status
```

**Example:**
```
> list nodes
Node 1: SN 0x1234 - Last seen: 17:29:58, Calibration: Valid (2025-05-20)
Node 2: SN 0x5678 - Last seen: 17:30:05, Calibration: None
Node 3: SN 0x9ABC - Last seen: 17:30:03, Calibration: Valid (2025-05-20)

> list calibrations
SN 0x1234 (Node 1): Hard Iron [1.2, -0.5, 0.8], Status: valid
SN 0x5678 (Node 2): No calibration data
SN 0x9ABC (Node 3): Hard Iron [0.8, 0.2, -0.3], Status: valid
```

#### Save/Load Session
```
save session
load session
clear calibrations
```

#### Help
```
help
help calibrate
```

---

## Node Discovery & Handshake

### Initial Handshake Packet (Node → Base → Jetson)
```
[Packet Type: 0x00] (HANDSHAKE)
[SN: 2 bytes]
[Node_ID (assigned by base): 1 byte]
[Firmware Version: 2 bytes]
[Battery Level: 1 byte]
```

### Jetson Behavior on Handshake
1. Parse handshake packet.
2. Extract SN and node_id.
3. Update session: `node_id_to_sn[node_id] = SN`.
4. If SN exists in calibrations dictionary, prepare to send calibration data on wake-up.
5. Log: `[17:30:20.456] Node 1 (SN: 0x1234) connected. Calibration data available: Yes`.

---

## Command Handling Flow

### Sending a Command

```
User Input: "calibrate node 1"
    │
    ├─ Parse: node_id=1, cmd=CALIBRATE
    │
    ├─ Lookup SN: node_id_to_sn[1] = 0x1234
    │
    ├─ Build packet:
    │   [0x10][0x1234][0x06] = CALIBRATE packet
    │
    ├─ Send to base station (serial/LoRa)
    │
    ├─ Add to command_queue:
    │   {"type": "CALIBRATE", "sn": 0x1234, "sent_at": time, "acked": False}
    │
    └─ Display: "[Sent] CALIBRATE to Node 1. Waiting for ACK..."
```

### Receiving an ACK

```
Incoming packet: [0x13][0x10][0x1234][0x00]
    │
    ├─ Parse: ACK, command_type=CALIBRATE, SN=0x1234, status=received
    │
    ├─ Lookup node_id: sn_to_node_id[0x1234] = 1
    │
    ├─ Find in command_queue and mark: acked=True
    │
    ├─ Update node_status:
    │   node_status[1]["calibrating"] = True
    │
    └─ Display: "[17:30:10.450] ACK received: Node 1 acknowledged CALIBRATE. Calibrating for 60s..."
```

### Receiving Calibration Data

```
Incoming packet: [0x12][0x1234][hard_iron][soft_iron][timestamp][status]
    │
    ├─ Parse: CALIBRATION_DATA, SN=0x1234, offsets={...}, matrix={...}, status=success
    │
    ├─ Lookup node_id: sn_to_node_id[0x1234] = 1
    │
    ├─ Update calibrations:
    │   calibrations[0x1234] = {
    │       "hard_iron": [...],
    │       "soft_iron": [...],
    │       "timestamp": ...,
    │       "status": "valid"
    │   }
    │
    ├─ Update node_status:
    │   node_status[1]["calibrating"] = False
    │
    ├─ Save session to disk
    │
    └─ Display:
       "[17:30:70.452] CALIBRATION_DATA received from Node 1:
        - Hard Iron: [1.2, -0.5, 0.8]
        - Soft Iron: [[0.98, 0.01, 0.02], ...]
        - Status: success
       [17:30:70.500] Calibration data for SN 0x1234 saved to session."
```

---

## Implementation Considerations

### Thread Safety
- Use thread-safe data structures (queues, locks) for shared session state.
- Command queue for incoming packets to avoid race conditions.

### Timeout & Retry
- If ACK not received within 5 seconds, display warning: `[Warning] No ACK from Node 1 after 5s.`
- No automatic retry; user must manually resend command.

### Error Handling
- Invalid node_id: `[Error] Node ID 99 not found. Use 'list nodes' to see active nodes.`
- Malformed commands: `[Error] Invalid command. Type 'help' for usage.`
- Packet parse errors: Log and display, but don't crash.

### Logging
- All events (packet received, command sent, ACK, etc.) logged to file and displayed.
- Log file: `~/.smartfires/jetson.log` (rotated daily).

### Graceful Shutdown
- On exit, save session to disk.
- Close serial/LoRa connections cleanly.

---

## Dependencies & Tools
- Python (threading, json, serial/LoRa drivers)
- Curses or similar for split-screen UI (or simpler print-based approach for MVP)

---

## Next Steps
1. Implement packet parsing and routing for CALIBRATE, RESET, CALIBRATION_DATA, ACK.
2. Implement session persistence (load/save JSON).
3. Implement CLI with split-screen display (start with print-based MVP).
4. Implement command parsing and transmission.
5. Test end-to-end: send calibrate → receive ACK → receive calibration data → save session.

---

*This design will be refined as implementation details emerge.*
