# Jetson CLI and Command System Design

## Overview

A split-screen interactive CLI on the Jetson for viewing live packet data, sending commands
(CALIBRATE, RESET) to nodes, managing calibration storage, and displaying computed node
orientation. All calibration fitting and heading computation run on the Jetson — the node
transmits only raw IMU data and a statistical calibration summary.

---

## Node Identity

Nodes are identified by `uid_hash` — the 32-bit FNV-1a hash of the SAMD21's factory-burned
128-bit unique serial number (hardware registers 0x0080A00C to 0x0080A048). This value is
embedded in every PKT_AWAKEN payload and is unique per board. Collision probability for
fewer than 50 nodes is negligible (~0.0000003%).

The base station assigns a runtime `node_id` (1-based integer) from uid_hash on first
AWAKEN, which is used for LoRa routing. The Jetson maintains the uid_hash <-> node_id
mapping in session.json.

**No HANDSHAKE redesign is needed.** The existing AWAKEN -> TIME_SYNC flow already handles
node discovery and ID assignment. The Jetson only needs to parse uid_hash from the forwarded
AWAKEN payload to populate its mapping.

---

## Architecture

```
Jetson Receiver Process
────────────────────────────────────────────────────────────
  Packet Listener Thread
  Reads UART frames, decodes all packet types,
  posts events to queue.Queue
      │
      ▼
  Session Manager  (thread-safe, shared state, threading.Lock)
  uid_hash <-> node_id mapping
  calibrations dict  { uid_hash -> { hard_iron, soft_iron } }
  node_status dict   { node_id -> last_seen, heading, pitch, roll }
  command_queue      [ { type, node_id, sent_at, acked } ]
      │                             │
      ▼                             ▼
  UI/Display Thread          Command Handler Thread
  curses split screen        Parses user input
  Packet log (top 80%)       Builds + sends UART frames
  Command input (bottom)     Tracks ACK receipt + timeouts
────────────────────────────────────────────────────────────
```

---

## Packet Types

### Existing (unchanged)

| Type | Value | Direction | Description |
| --- | --- | --- | --- |
| PKT_AWAKEN | 0x06 | Node -> Jetson | Boot handshake; contains uid_hash |
| PKT_BUNDLE | 0x04 | Node -> Jetson | Telemetry bundle (15 samples) |
| PKT_STATUS | 0x05 | Node -> Jetson | GPS + battery + raw IMU, every 15 min |
| PKT_TIME_SYNC | 0x03 | Jetson -> Nodes | Session clock broadcast |
| PKT_ACK_SUMMARY | 0x07 | Jetson -> Node | App-layer reliability bitmap |

### New Packet Types

#### CMD_CALIBRATE (0x10) — Jetson -> Base -> Node

Triggers the 60-second calibration routine on the target node.

```
[PktHeader:   4]   pkt_type=0x10, node_id=0, seq
[node_id:     1]   target node (base uses as LoRa radio address)
[duration_s:  1]   calibration window in seconds (default 0x3C = 60)
[CRC-8:       1]
LoRa payload total: 7 bytes
```

Flow:

1. Jetson wraps packet in UART frame, sends to base.
2. Base sees pkt_type=0x10, extracts node_id, calls `sendToWait(payload, len, node_id)`.
3. Node receives, sends CMD_ACK (status=processing), enters calibration state.
4. Node collects raw IMU for duration_s, computes statistical summary, sends CALIBRATION_DATA.
5. Node resumes normal BUNDLE + STATUS transmission.

#### CMD_RESET (0x11) — Jetson -> Base -> Node

```
[PktHeader:   4]   pkt_type=0x11, node_id=0, seq
[node_id:     1]   target node
[reset_type:  1]   0x00=soft reset, 0x01=hard reset (clears RAM state)
[CRC-8:       1]
LoRa payload total: 7 bytes
```

#### CALIBRATION_DATA (0x12) — Node -> Base -> Jetson

Statistical summary from a 60-second calibration session. Single packet; the Jetson
derives hard iron offsets and a full 3x3 soft iron matrix from this data.

```
[PktHeader:    4]   pkt_type=0x12, node_id=sender, seq
[uid_hash:     4]   sender's uid_hash (for verification against session mapping)
[sample_count: 2]   number of samples collected (uint16)
[mag_mean:    12]   3 x float32 (x, y, z) — centroid; used as hard iron offset
[mag_cov:     24]   6 x float32 — upper triangle of 3x3 covariance (xx,yy,zz,xy,xz,yz)
[mag_min:     12]   3 x float32 — per-axis minimum (quality validation)
[mag_max:     12]   3 x float32 — per-axis maximum (quality validation)
[status:       1]   0x00=success, 0x01=low_sample_count, 0x02=error
[CRC-8:        1]
LoRa payload total: 72 bytes
```

#### CMD_ACK (0x13) — Node -> Base -> Jetson

Node acknowledgment of a received command.

```
[PktHeader:  4]   pkt_type=0x13, node_id=sender, seq
[cmd_type:   1]   command being acknowledged (0x10=CALIBRATE, 0x11=RESET)
[uid_hash:   4]   sender's uid_hash
[status:     1]   0x00=received, 0x01=processing, 0x02=error
[CRC-8:      1]
LoRa payload total: 11 bytes
```

---

## Extended StatusPayload (24 bytes, was 12)

Raw magnetometer and accelerometer are appended to the existing STATUS fields. Heading is
computed by the Jetson on receipt — it is not transmitted by the node.

```c
struct __attribute__((packed)) StatusPayload {
    int32_t  lat_e7;      // degrees x 1e7     (valid if STATUS_GPS_VALID)
    int32_t  lon_e7;      // degrees x 1e7     (valid if STATUS_GPS_VALID)
    uint16_t battery_mv;  // millivolts         (valid if STATUS_BATT_VALID)
    uint8_t  battery_pct; // 0-100              (valid if STATUS_BATT_VALID)
    uint8_t  flags;
    int16_t  mag_x;       // uT x 10, raw      (valid if STATUS_IMU_VALID)
    int16_t  mag_y;
    int16_t  mag_z;
    int16_t  accel_x;     // mg (milli-g), raw (valid if STATUS_IMU_VALID)
    int16_t  accel_y;
    int16_t  accel_z;
};

static constexpr uint8_t STATUS_GPS_VALID  = 0x01;
static constexpr uint8_t STATUS_BATT_VALID = 0x02;
static constexpr uint8_t STATUS_IMU_VALID  = 0x04;
```

STATUS LoRa payload: 4 + 24 + 1 = **29 bytes** (was 17).

---

## Session State

### In-Memory Structure

```python
session = {
    "node_id_to_uid_hash": {
        1: 0xA1B2C3D4,
        2: 0xE5F60718,
    },
    "uid_hash_to_node_id": {
        0xA1B2C3D4: 1,
        0xE5F60718: 2,
    },
    "calibrations": {
        0xA1B2C3D4: {
            "hard_iron":    [1.23, -0.45, 0.78],       # shape (3,)
            "soft_iron":    [[0.98, 0.02, -0.01],       # shape (3,3)
                             [0.02, 1.01,  0.00],
                             [-0.01, 0.00, 0.97]],
            "sample_count": 587,
            "timestamp":    1748000000,
            "status":       "valid"
        },
    },
    "command_queue": [
        {"type": "CALIBRATE", "node_id": 1, "sent_at": 1748001000, "acked": False},
    ],
    "node_status": {
        1: {
            "last_seen":       1748001234,
            "calibrating":     False,
            "heading_true_deg": 247.3,
            "pitch_deg":        2.1,
            "roll_deg":        -1.4,
            "last_heading_ts": 1748001234,
        },
    },
}
```

### Persistence: `~/.smartfires/session.json`

Loaded at startup. Written after each CALIBRATION_DATA received, and on `save session`
command. The calibrations dict is the single source of truth — never deleted without an
explicit `clear calibration` command.

---

## AWAKEN Handling (Updated)

When the Jetson receives a forwarded PKT_AWAKEN:

```
Incoming AWAKEN: [PktHeader(node_id=1)][AwakenPayload(uid_hash=0xA1B2C3D4)]
    │
    ├── Parse uid_hash from AwakenPayload
    ├── Extract node_id from PktHeader.node_id  (base assigned it)
    ├── Update session: node_id_to_uid_hash[1] = 0xA1B2C3D4
    │
    ├── Look up calibrations[0xA1B2C3D4]
    │
    ├── Found:
    │   Log: "[17:30:20] Node 1 (uid=0xA1B2C3D4) connected. Calibration on file."
    │   (No push needed — calibration is applied server-side on STATUS receipt)
    │
    └── Not found:
        Log: "[17:30:20] Node 1 (uid=0xA1B2C3D4) connected. No calibration — use 'calibrate node 1'."
```

Note: CALIBRATION_PUSH (0x14) is not used. The Jetson holds and applies calibration
entirely on its own side when STATUS packets arrive. No data needs to be sent to the node.

---

## Heading Computation on STATUS Receipt

```python
def compute_heading(status: dict, calibration: dict) -> dict:
    mag_raw   = np.array([status["mag_x"], status["mag_y"], status["mag_z"]]) / 10.0  # uT
    accel_raw = np.array([status["accel_x"], status["accel_y"], status["accel_z"]]) / 1000.0  # g

    hard_iron = np.array(calibration["hard_iron"])
    soft_iron = np.array(calibration["soft_iron"])

    # Calibrate magnetometer
    mag_c = soft_iron @ (mag_raw - hard_iron)

    # Tilt from accelerometer
    roll  = np.arctan2(accel_raw[1], accel_raw[2])
    pitch = np.arctan2(-accel_raw[0], np.sqrt(accel_raw[1]**2 + accel_raw[2]**2))

    # Tilt-compensated heading
    mx_h = mag_c[0]*np.cos(pitch) + mag_c[2]*np.sin(pitch)
    my_h = (mag_c[0]*np.sin(roll)*np.sin(pitch)
           + mag_c[1]*np.cos(roll)
           - mag_c[2]*np.sin(roll)*np.cos(pitch))

    heading_mag  = np.degrees(np.arctan2(-my_h, mx_h)) % 360
    declination  = magnetic_declination(status["lat"], status["lon"])  # WMM table
    heading_true = (heading_mag + declination) % 360

    return {
        "heading_true_deg": round(heading_true, 1),
        "pitch_deg":        round(np.degrees(pitch), 1),
        "roll_deg":         round(np.degrees(roll), 1),
    }
```

---

## Base Station Changes

`SmartFiresBaseApp::handleJetsonCommandPayload()` is extended to handle new types:

| pkt_type | Action |
| --- | --- |
| 0x03 PKT_TIME_SYNC | Cache Jetson time (existing) |
| 0x07 PKT_ACK_SUMMARY | Forward to node via send() (existing) |
| 0x10 CMD_CALIBRATE | Extract node_id, forward via sendToWait(payload, len, node_id) |
| 0x11 CMD_RESET | Extract node_id, forward via sendToWait(payload, len, node_id) |

`processIncomingLoRa()` adds CALIBRATION_DATA (0x12) and CMD_ACK (0x13) to its
forward-to-Jetson routing (encodeBaseFrame), alongside existing BUNDLE/STATUS/AWAKEN.

---

## CLI Interface

### Split-Screen Layout

```
===========================================================================
                        SMARTFIRES JETSON CLI
===========================================================================
 [PACKET LOG]

 [17:30:01.234] STATUS  Node 1 (0xA1B2C3D4)  hdg=247.3 deg  pitch=2.1 deg
                         T=25.0C H=45%  batt=3800mV  gps=valid
 [17:30:02.456] BUNDLE  Node 2 (0xE5F60718)  15 samples  T=26.1C
 [17:30:10.234] CMD_ACK Node 1  cmd=CALIBRATE  status=processing
                         (calibrating... ~60s remaining)
 [17:31:10.789] CALIB   Node 1  samples=587  hard=[1.23,-0.45,0.78]  OK
                         soft_iron eigenvalues=[0.98, 1.01, 0.97]

---------------------------------------------------------------------------
 > calibrate node 1
 [Sent] CALIBRATE to Node 1 (uid=0xA1B2C3D4). Waiting for ACK...
 >
```

### Commands

```
calibrate node <id>       Send CMD_CALIBRATE; wait for ACK + CALIBRATION_DATA
calibrate all             Calibrate all active nodes sequentially
cal <id>                  Shorthand for calibrate node <id>

reset node <id>           Soft reset target node
reset node <id> hard      Hard reset (clears node RAM state)

list nodes                node_id, uid_hash, last_seen, calibration status, heading
list calibrations         Stored calibration params per uid_hash
status                    Alias for list nodes

save session              Write session.json immediately
load session              Reload session.json from disk
clear calibration <id>    Remove calibration for one node (requires confirmation)
clear calibrations        Remove all calibrations (requires confirmation)

help [command]            Display command syntax and examples
```

### Example: Calibration Flow

```
> calibrate node 1
[17:30:10.234] Sending CALIBRATE to Node 1 (uid=0xA1B2C3D4), duration=60s...
[17:30:10.500] CMD_ACK: Node 1 acknowledged CALIBRATE — status=processing.
               Rotate the node through all orientations for 60 seconds.
               (Node will be silent on LoRa while calibrating)
[17:31:10.823] CALIBRATION_DATA received from Node 1:
               Samples:     587
               Mag range:   x=[18.3, 82.1] y=[-44.2, 31.6] z=[-12.8, 67.4]
               Eigenvalues: [0.98, 1.01, 0.97]  (close to 1.0 = good spherical fit)
               Status:      success
[17:31:10.850] Calibration stored for uid=0xA1B2C3D4. Heading will be computed
               on next STATUS packet (within 15 min).

> list nodes
Node 1  uid=0xA1B2C3D4  last_seen=17:31:10  calib=valid (2026-05-20)  hdg=247.3 deg
Node 2  uid=0xE5F60718  last_seen=17:30:05  calib=none                hdg=--
```

---

## Command Handling Detail

### Sending a Command

```
User types: "calibrate node 1"
    │
    ├── Verify node_id=1 is in session (node_id_to_uid_hash[1] exists)
    ├── Build CMD_CALIBRATE: [0xA5][0x10][0x00][seq][0x01][0x3C][crc]
    ├── Wrap in UART frame and write to serial
    ├── Add to command_queue: {type=CALIBRATE, node_id=1, sent_at=now, acked=False}
    └── Display: "[Sent] CALIBRATE to Node 1. Waiting for ACK..."
```

### Receiving CMD_ACK

```
Incoming CMD_ACK: pkt_type=0x13, node_id=1, cmd_type=0x10, uid_hash=0xA1B2C3D4
    │
    ├── Verify uid_hash matches session entry for node_id=1
    ├── Mark command_queue entry acked=True
    ├── Update node_status[1]["calibrating"] = True
    └── Display: "CMD_ACK: Node 1 acknowledged CALIBRATE — status=processing."
```

### Receiving CALIBRATION_DATA

```
Incoming CALIBRATION_DATA: pkt_type=0x12, node_id=1, uid_hash, stats...
    │
    ├── Verify uid_hash matches session mapping for node_id=1
    ├── Run quality checks (sample count >= 200, axis range >= 20 uT, eigenvalues > 0)
    ├── Compute via numpy:
    │     hard_iron = mag_mean
    │     soft_iron = V @ diag(1/sqrt(eigenvalues)) @ V.T  (from eigh(covariance))
    ├── Store calibrations[uid_hash] = { hard_iron, soft_iron, sample_count, timestamp }
    ├── Update node_status[1]["calibrating"] = False
    ├── Save session.json
    └── Display calibration summary + eigenvalues
```

### Receiving STATUS (with IMU data)

```
Incoming STATUS: pkt_type=0x05, node_id=1, GPS + battery + mag + accel
    │
    ├── Parse all fields (existing GPS/battery + new mag/accel int16 fields)
    ├── Look up calibrations[uid_hash]
    │
    ├── If calibration found and STATUS_IMU_VALID set:
    │     Compute heading (tilt-compensated + declination corrected)
    │     Update node_status[1]: heading, pitch, roll, last_heading_ts
    │     Log to status JSONL with heading fields
    │
    └── If no calibration:
          Log STATUS without heading; display calibration prompt if not already shown
```

---

## Implementation Notes

### Thread Safety

- All session state accessed from Listener, UI, and Command threads via `threading.Lock`.
- Use `queue.Queue` for event passing from Listener to UI thread.

### Timeout Handling

- CMD_ACK not received within 5 s: `[Warning] No ACK from Node 1 after 5s.`
- CALIBRATION_DATA not received within `(duration_s + 15)` s after CMD_ACK:
  `[Warning] No CALIBRATION_DATA from Node 1 — calibration may have failed.`
- No automatic retry — user resends manually.

### Error Handling

- Unknown node_id: `[Error] Node 3 not found. Use 'list nodes' to see active nodes.`
- uid_hash mismatch on CALIBRATION_DATA: log and discard; do not overwrite stored data.
- Degenerate covariance (eigenvalue <= 0): reject calibration with explanation.
- Malformed command: `[Error] Invalid syntax. Type 'help calibrate'.`

### Logging

- All events appended to `~/.smartfires/jetson.log` (daily rotation).
- Heading included in status JSONL and telemetry CSV rows when computed.

### UI Library

- Python `curses` for split-screen (available on Jetson Linux).
- MVP fallback: scrolling print output with readline prompt if curses is problematic.

---

## Dependencies

```
Python 3.10+
numpy         — calibration eigendecomposition and heading computation
pyserial      — UART communication
curses        — split-screen terminal UI (stdlib)
threading     — concurrency (stdlib)
queue         — thread-safe event passing (stdlib)
json          — session persistence (stdlib)
struct        — packet encoding/decoding (stdlib)
```
