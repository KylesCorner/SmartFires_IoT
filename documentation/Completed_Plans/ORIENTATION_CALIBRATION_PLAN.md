---
name: orientation-calibration-plan
description: Plan for Jetson-side IMU calibration (ellipsoid fitting, soft/hard iron correction, declination) computing absolute heading from raw mag/accel data sent by the node.
category: plan-completed
status: historical
superseded_by: dmp-cleanup-plan
related_docs:
  - jetson-cli-and-command-system
  - deployment-schedule
---

# Node Orientation Calibration and Absolute Heading Plan

## Overview

Each node carries an ICM-20948 9-DOF IMU (magnetometer + accelerometer). The node's role
is limited to reading and transmitting raw sensor data. All calibration computation and
heading calculation are performed on the Jetson, which has the processing power for full
ellipsoid fitting and magnetic declination correction.

**Node responsibilities:** Read raw IMU, run the 60-second data-collection window on command,
compute a compact statistical summary, transmit it once, then include raw mag + accel in
every subsequent STATUS packet.

**Jetson responsibilities:** Receive the statistical summary, compute hard iron offsets and
a full 3x3 soft iron matrix via eigendecomposition, store calibration per uid_hash, and
apply calibration + tilt compensation + magnetic declination to compute heading from each
STATUS packet.

---

## Design Decisions

| Decision | Choice | Rationale |
| --- | --- | --- |
| Where calibration is computed | Jetson | Full ellipsoid fitting via numpy eigendecomposition; not feasible on SAMD21 |
| What node sends during calibration | Statistical summary (mean + covariance) | Single 72-byte packet; O(N) running math on node; gives Jetson all it needs for full 3x3 soft iron |
| What node sends in STATUS | Raw mag (x,y,z) + raw accel (x,y,z) as int16 | 12 bytes added; Jetson applies stored calibration at receive time |
| Gyroscope | Not transmitted | Not needed for tilt-compensated heading on stationary nodes |
| IMU update rate | Every STATUS packet (15 min) | Nodes are stationary once deployed; heading changes only on disturbance |
| Calibration storage | Jetson only (session.json) | Node holds no calibration parameters; no CALIBRATION_PUSH needed |
| Node identifier | uid_hash (32-bit FNV-1a of SAMD21 128-bit serial) | Hardware-unique; already in AWAKEN payload; negligible collision risk |

---

## Data Flow

```
First-time calibration
──────────────────────────────────────────────────────────────────────────

Jetson CLI: `calibrate node <id>`
    │
    ▼
Jetson sends CMD_CALIBRATE (0x10) → Base Station → Node (LoRa)
    │
    ▼
Node: sends CMD_ACK (status=processing), enters calibration state
    │  60 seconds: rotate node through all orientations (figure-8, tumble)
    │  Sample mag + accel at ~10 Hz (600 samples)
    │  Compute running statistics (Welford online algorithm):
    │    - mag mean, min, max per axis
    │    - mag sample covariance matrix upper triangle (6 values)
    │    - sample count
    ▼
Node sends CALIBRATION_DATA (0x12) — single 72-byte packet — to Jetson
    │
    ▼
Jetson:
    - Hard iron offset  = mag_mean (centroid of the ellipsoid)
    - Soft iron matrix  = eigendecomposition of covariance matrix (full 3x3)
    - Validates quality (sample count, axis range, covariance rank)
    - Stores { uid_hash → { hard_iron, soft_iron, timestamp } } in session.json
    - Displays result in CLI

Normal operation (every 15 min STATUS)
──────────────────────────────────────────────────────────────────────────

Node sends PKT_STATUS with raw mag_x/y/z and accel_x/y/z
    │
    ▼
Jetson:
    1. mag_c = soft_iron @ (mag_raw - hard_iron)   (calibration)
    2. roll, pitch from accel                       (tilt)
    3. rotate mag_c into horizontal plane           (tilt compensation)
    4. heading_mag = atan2(-my_h, mx_h)             (magnetic heading)
    5. heading_true = heading_mag + declination(lat, lon)  (true heading)
    6. Store heading in session; display in CLI; log to CSV
```

---

## Calibration: Statistical Summary

During the 60-second window the node computes running statistics using the Welford online
algorithm — a single pass through the data, O(N) time, no matrix operations required on
the SAMD21.

### Node-side computation (~10 Hz, 600 samples)

```c
// Welford online algorithm for mean and sum of squared deviations
for each sample (mag_x, mag_y, mag_z):
    n++
    delta[i]  = sample[i] - mean[i]
    mean[i]  += delta[i] / n
    delta2[i] = sample[i] - mean[i]
    M2[i][j] += delta[i] * delta2[j]   // accumulate cross-products for covariance

// At end: sample covariance matrix upper triangle
cov[i][j] = M2[i][j] / (n - 1)
```

This produces the full statistical summary in a single pass with constant memory:

- `mag_mean[3]` — centroid of the ellipsoid; used directly as hard iron offset
- `mag_cov[6]` — upper triangle of 3x3 covariance (xx, yy, zz, xy, xz, yz); encodes ellipsoid shape
- `mag_min[3]`, `mag_max[3]` — axis ranges for quality validation
- `sample_count` — for quality checking

### Jetson-side computation (numpy, runs once on CALIBRATION_DATA receipt)

```python
import numpy as np

# Reconstruct symmetric covariance matrix from upper triangle
C = np.array([[cov_xx, cov_xy, cov_xz],
              [cov_xy, cov_yy, cov_yz],
              [cov_xz, cov_yz, cov_zz]])

# Eigendecomposition — columns of V are principal axes of the ellipsoid
eigenvalues, V = np.linalg.eigh(C)

# Soft iron matrix: maps ellipsoid back to sphere
scales = 1.0 / np.sqrt(np.maximum(eigenvalues, 1e-6))
soft_iron = V @ np.diag(scales) @ V.T   # full 3x3

# Hard iron = centroid of magnetometer distribution
hard_iron = mag_mean  # shape (3,)
```

### Per-STATUS heading computation (Jetson, runs on every STATUS received)

```python
# 1. Apply calibration
mag_c = soft_iron @ (mag_raw - hard_iron)

# 2. Tilt from accelerometer
roll  = np.arctan2(accel_y, accel_z)
pitch = np.arctan2(-accel_x, np.sqrt(accel_y**2 + accel_z**2))

# 3. Tilt-compensated magnetometer (horizontal plane projection)
mx_h = mag_c[0]*np.cos(pitch) + mag_c[2]*np.sin(pitch)
my_h = (mag_c[0]*np.sin(roll)*np.sin(pitch)
       + mag_c[1]*np.cos(roll)
       - mag_c[2]*np.sin(roll)*np.cos(pitch))

# 4. Heading
heading_mag  = np.degrees(np.arctan2(-my_h, mx_h)) % 360
heading_true = (heading_mag + magnetic_declination(lat, lon)) % 360
```

**Magnetic declination:** Computed from GPS lat/lon using the WMM (World Magnetic Model)
lookup table (~2 KB ROM). In Northern California, declination is approximately 12.5 deg East.
Correcting this eliminates the otherwise permanent ~12 deg offset from true north.

---

## Wire Protocol

### CMD_CALIBRATE (0x10) — Jetson → Base → Node

```
[PktHeader:   4]   pkt_type=0x10, node_id=0, seq
[node_id:     1]   target node (base uses as LoRa radio address)
[duration_s:  1]   calibration window in seconds (default 0x3C = 60)
[CRC-8:       1]
LoRa payload total: 7 bytes
```

### CMD_RESET (0x11) — Jetson → Base → Node

```
[PktHeader:   4]   pkt_type=0x11, node_id=0, seq
[node_id:     1]   target node
[reset_type:  1]   0x00=soft reset, 0x01=hard reset
[CRC-8:       1]
LoRa payload total: 7 bytes
```

### CALIBRATION_DATA (0x12) — Node → Base → Jetson

Single packet. Contains everything the Jetson needs for full ellipsoid fitting.

```
[PktHeader:    4]   pkt_type=0x12, node_id=sender, seq
[uid_hash:     4]   sender's uid_hash (for verification)
[sample_count: 2]   number of samples collected (uint16)
[mag_mean:    12]   3 x float32 (x, y, z) — centroid; hard iron estimate
[mag_cov:     24]   6 x float32 — upper triangle of 3x3 covariance (xx,yy,zz,xy,xz,yz)
[mag_min:     12]   3 x float32 — per-axis minimum (quality check)
[mag_max:     12]   3 x float32 — per-axis maximum (quality check)
[status:       1]   0x00=success, 0x01=low_sample_count, 0x02=error
[CRC-8:        1]
LoRa payload total: 72 bytes
```

### CMD_ACK (0x13) — Node → Base → Jetson

```
[PktHeader:  4]   pkt_type=0x13, node_id=sender, seq
[cmd_type:   1]   command being acknowledged (0x10=CALIBRATE, 0x11=RESET)
[uid_hash:   4]   sender's uid_hash
[status:     1]   0x00=received, 0x01=processing, 0x02=error
[CRC-8:      1]
LoRa payload total: 11 bytes
```

### Extended StatusPayload (24 bytes, was 12)

Adds raw magnetometer and accelerometer readings. Heading is computed by the Jetson after
receipt — it is not transmitted.

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

**Encoding:**

- Magnetometer: `int16 = (uT value) * 10` — range +-3277 uT at 0.1 uT resolution.
  Earth's field is ~50 uT so headroom is ample.
- Accelerometer: `int16 = mg value` — range +-32.7 g at 1 mg resolution.
  Sufficient for tilt measurement (+-1g at full tilt).

**STATUS LoRa payload:** 4 (header) + 24 (payload) + 1 (CRC) = **29 bytes** (was 17).

---

## SensorSnapshot Additions

```c
struct SensorSnapshot {
    // ... existing fields (wind, temp, humidity, PM, GPS, battery) ...

    // Raw IMU — populated by Icm20948Sensor::fillSnapshot()
    // Encoding matches StatusPayload above
    int16_t magX     = 0;   // uT x 10
    int16_t magY     = 0;
    int16_t magZ     = 0;
    int16_t accelX   = 0;   // mg
    int16_t accelY   = 0;
    int16_t accelZ   = 0;
    bool    imuValid = false;
};
```

---

## Node Calibration State Machine

```
State: IDLE
    Normal sensing: Icm20948Sensor::fillSnapshot() populates raw mag + accel
    STATUS packets transmit raw IMU fields every 15 min
    │
    │  recv CMD_CALIBRATE
    ▼
State: CALIBRATING
    - Send CMD_ACK (status=processing)
    - Suspend BUNDLE transmission (~60s gap is safe; TDMA stale-sync threshold is 22 min)
    - Sample ICM-20948 magnetometer at ~10 Hz for duration_s seconds
    - Accumulate Welford online statistics: mean, cross-product sums for covariance, min, max
    - On completion:
        - Finalise covariance upper triangle from accumulated sums
        - Encode and transmit single CALIBRATION_DATA packet (72 bytes)
    - Return to IDLE; BUNDLE + STATUS transmission resumes
```

The node holds no calibration parameters at any point. CALIBRATION_PUSH (0x14) is not used.

---

## Calibration Quality Checks (Jetson-side)

Before storing calibration the Jetson validates:

| Check | Criterion | Action on fail |
| --- | --- | --- |
| Sample count | >= 200 | Warn user; store with status=low_sample_count |
| Axis range | max - min >= 20 uT on all axes | Warn: insufficient rotation during calibration |
| Covariance rank | All eigenvalues > 0 | Reject: degenerate (planar) data |
| Hard iron magnitude | magnitude(hard_iron) < 200 uT | Sanity check against Earth-scale fields |

---

## Calibration Storage on Jetson

Persistent file: `~/.smartfires/session.json`

```json
{
    "node_id_to_uid_hash": { "1": "0xA1B2C3D4" },
    "calibrations": {
        "0xA1B2C3D4": {
            "hard_iron": [1.23, -0.45, 0.78],
            "soft_iron": [
                [0.98,  0.02, -0.01],
                [0.02,  1.01,  0.00],
                [-0.01, 0.00,  0.97]
            ],
            "sample_count": 587,
            "timestamp": 1748000000,
            "status": "valid"
        }
    },
    "node_status": {
        "1": {
            "last_seen": 1748001234,
            "calibrating": false,
            "heading_true_deg": 247.3,
            "pitch_deg": 2.1,
            "roll_deg": -1.4,
            "last_heading_ts": 1748001234
        }
    },
    "last_updated": 1748001234
}
```

---

## Expected Heading Accuracy

| Error source | Contribution |
| --- | --- |
| Sensor noise (AK09916, single STATUS sample) | ~4-5 deg |
| Hard iron residual (mean-centroid method) | ~0.5-1 deg |
| Soft iron residual (full 3x3 eigen, good rotation coverage) | ~0.5-2 deg |
| Tilt compensation (stationary node, accel noise) | ~0.2-0.5 deg |
| Magnetic declination (corrected via WMM + GPS lat/lon) | <0.1 deg |
| Local environmental anomalies (metal, rock) | Unpredictable |

**Practical expectation: +-2 to 5 deg (1-sigma) after calibration with adequate rotation
coverage and declination correction applied.** This is a meaningful improvement over a
diagonal-only soft iron approach (+-5 to 8 deg) and eliminates the fixed ~12 deg true-north
offset that would otherwise exist without declination correction.

The dominant remaining variable is rotation coverage during the 60-second calibration window.
If the operator does not cover all orientations, the covariance matrix will be rank-deficient
and the Jetson will reject or warn about the calibration.
