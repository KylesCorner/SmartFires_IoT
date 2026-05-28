# Plan: #2 Sensor-to-Body Alignment Matrix

Goal: eliminate persistent yaw compression caused by three distinct but interacting
problems in the heading pipeline. The fix must be applied in a specific order because
each problem compounds the next.

---

## Observed Mismatch Summary

- During bench rotation testing, physical turns of about 90 deg were repeatedly observed
  to produce smaller heading deltas on Jetson (often under 75 deg).
- Raw IMU stream (`src=IMU`, token `imu_raw_sample`) showed substantial magnetometer
  vector movement across turns, confirming the issue is in the heading math pipeline,
  not in data acquisition or transmission.

Captured raw samples from the troubleshooting session:

- `16:48:19` `mag_ut=[100.500,-76.650,174.900] accel_mg=[24.4,-13.7,1012.2] gyro_dps=[0.534,1.107,-1.198]`
- `16:48:46` `mag_ut=[81.450,64.950,196.950] accel_mg=[14.6,-31.2,1026.9] gyro_dps=[-0.275,-1.397,0.244]`
- `16:50:56` `mag_ut=[-55.800,54.150,158.550] accel_mg=[3.9,-15.6,1020.0] gyro_dps=[0.931,-2.916,-0.122]`

Heading proxy from raw atan2(-Y, X) on the horizontal components:

- Sample 1 to 2: −75.9 deg.
- Sample 2 to 3: −97.3 deg.

The accel vectors confirm the board was nearly flat (roll < 2 deg, pitch < 2 deg) during
all three samples. This rules out physical tilt as the cause of the compression.

---

## Root Cause Analysis

There are three distinct problems. They must be understood separately because the
original plan conflated them, which led to an incorrect fix order.

### Problem A — Soft-Iron Sphere Is Not Aligned With the Body Frame (primary cause of compression)

The eigendecomposition-based soft iron calibration (`session.py: on_calibration_data`)
computes:

```python
eigenvalues, V = np.linalg.eigh(C)
scales = 1.0 / np.sqrt(np.maximum(eigenvalues, 1e-6))
soft_iron = V @ np.diag(scales) @ V.T
```

This maps the magnetometer's calibration ellipsoid into a unit sphere. The sphere is
geometrically correct in 3D — but its principal axes (the eigenvectors in V) are
oriented in whatever direction the covariance data happened to point. There is no
constraint that the sphere's vertical axis is parallel to gravity.

After applying soft iron, `mag_c = soft_iron @ (mag_raw - hard_iron)` lives in the
eigenvector frame, not the sensor body frame. The tilt compensation that follows:

```python
mx_h = mag_c[0] * cos(pitch) + mag_c[2] * sin(pitch)
my_h = mag_c[0]*sin(roll)*sin(pitch) + mag_c[1]*cos(roll) - mag_c[2]*sin(roll)*cos(pitch)
```

assumes `mag_c` is expressed in the same frame as `accel` — specifically that index 0 is
body-forward, index 1 is body-right, index 2 is body-down. Because `mag_c` is in the
eigenvector frame, the horizontal plane the formula projects onto is tilted relative to
geographic horizontal. A 360-degree horizontal yaw then traces an ellipse in `atan2`
space rather than a circle, and heading deltas are compressed.

This is the dominant cause of the observed ~75 deg / 90 deg compression.

### Problem B — AK09916 Axes Are Intrinsically Different From ICM-20948 Axes (compounding misframe)

The SparkFun driver (`SparkfunIcm20948Driver.cpp`) passes `_imu.magX/Y/Z()` directly
from the AK09916 sub-chip and `_imu.accX/Y/Z()` from the ICM-20948 main chip. The
ICM-20948 datasheet documents that the AK09916 die is mounted rotated relative to the
main chip — the standard permutation is:

```
body_X_mag = AK09916_Y
body_Y_mag = AK09916_X
body_Z_mag = −AK09916_Z
```

The SparkFun library performs no axis remapping in raw (non-DMP) mode. As a result,
`mag_raw` sent over air and used in `_compute_heading` is in the AK09916 frame, while
`accel_raw` is in the ICM-20948 body frame. They are not in the same coordinate system.

With near-zero bench tilt this mainly creates a constant heading offset (because tilt
compensation is near-identity at small angles). At non-trivial pitch/roll the wrong-frame
tilt compensation introduces additional cross-axis coupling and worsens compression.

Note: because the calibration is also collected in the AK09916 frame, hard iron and
soft iron are internally consistent within that frame — the problem appears when
accel-derived roll/pitch is applied to AK09916-frame mag.

### Problem C — Magnetic Declination Stub Returns 0 Always (constant offset, unrelated to compression)

`_magnetic_declination` in `session.py` always returns `0.0` regardless of lat/lon. In
Northern California declination is approximately 12.5 deg East, so `heading_true` always
reads ~12.5 deg short of true north. This is a fixed offset on every reading and does
not contribute to compression.

---

## Fix Order

The fixes must be applied in this order because Problem B must be resolved before a
meaningful R_sb can be fitted, and Problem A requires R_sb to be in place before tilt
compensation is reliable.

```
Step 1: Apply fixed AK09916→body permutation  (eliminates Problem B)
Step 2: Fit and apply sensor-to-body matrix    (eliminates Problem A)
Step 3: Wire in real magnetic declination      (eliminates Problem C)
```

Applying R_sb before Step 1 would cause the Wahba solver to fit a near-permutation
matrix with floating-point noise rather than a clean body-frame rotation, and would
require separate R_sb matrices for mag and accel (since they start in different frames).
Getting Step 1 right first means mag and accel are in the same intrinsic frame when
the solver sees them.

---

## Step 1 — Apply the AK09916→ICM Body Permutation in the Heading Pipeline

**Location:** `session.py: _compute_heading` — immediately after decoding raw mag.  
**Scope:** Jetson only, no firmware changes required.

After decoding `mag_raw` (in uT, divided by 10) and before applying hard/soft iron:

```python
# Remap AK09916 frame → ICM-20948 body frame
# AK09916-X → body-Y, AK09916-Y → body-X, AK09916-Z → -body-Z
mag_body_frame = np.array([mag_raw[1], mag_raw[0], -mag_raw[2]])
# Proceed with mag_body_frame in place of mag_raw
mag_c = soft_iron @ (mag_body_frame - hard_iron)
```

Existing hard iron and soft iron calibration were collected in AK09916 frame. After
this change they will be applied in the wrong frame until recalibration is performed.
Therefore, after deploying Step 1, all nodes must be recalibrated so the Welford
statistics and resulting matrices are in the ICM-20948 body frame.

**Validation:** With any existing calibration cleared and identity soft iron, a 360-deg
horizontal yaw on a flat surface should produce a heading sweep that covers the full
0–360 range without compression. There may still be an angular offset (Problem A) but
compression should be substantially reduced or eliminated.

---

## Step 2 — Sensor-to-Body Alignment Matrix

After Step 1, mag and accel are both in the ICM-20948 body frame. There remains a
physical mounting offset between the ICM-20948 chip axes and the vehicle/drone body
axes (the Feather M0 board is rarely mounted perfectly forward-aligned). Step 2 fits a
rotation R_sb that maps the ICM-20948 frame into the vehicle body frame.

### 2a. Extend Persisted Calibration

Extend `session.json` calibration entries with a `sensor_to_body` field:

```json
{
    "0xA1B2C3D4": {
        "hard_iron": [...],
        "soft_iron": [[...], [...], [...]],
        "sensor_to_body": [[...], [...], [...]],
        "sample_count": 587,
        "timestamp": 1748000000,
        "status": "valid"
    }
}
```

Default to identity (`np.eye(3).tolist()`) when the field is absent for backward
compatibility with existing calibration files.

### 2b. Collect Alignment Dataset

The goal is to capture the same physical vectors in both sensor frame and known
body frame simultaneously, so R_sb can be fitted.

- Capture at least 6 static poses with known body orientation.  
  Recommended poses: forward-flat, right-flat, backward-flat, left-flat, nose-up, nose-down.
- For each pose, after pointing the vehicle body in a known direction, record:
  - Averaged `accel` (in ICM-20948 body frame — already correct after Step 1 firmware note)
  - Averaged `mag_c` after hard/soft iron correction (now in ICM-20948 body frame after Step 1)
- Reference vectors for each pose: the known gravity direction in body frame and, where
  applicable, the known magnetic field direction in body frame.
- Save pose-tagged samples for repeatable re-fit without reflashing nodes.

Because both vectors are now in the same ICM-20948 frame after Step 1, a single R_sb
correctly aligns both.

### 2c. Solve the Alignment Rotation

Use a Wahba/Kabsch-style least-squares fit:

```python
# observations: list of (sensor_vector, body_vector) pairs, unit-normalised
# sensor_vector: mag_c or accel from static pose, in ICM-20948 frame
# body_vector:   known reference direction in vehicle body frame

import numpy as np

def fit_sensor_to_body(observations):
    # Build the cross-covariance matrix H = sum(body_vec @ sensor_vec.T)
    H = sum(np.outer(b, s) for s, b in observations)
    U, _, Vt = np.linalg.svd(H)
    # Enforce proper rotation (det = +1, no reflections)
    d = np.linalg.det(U @ Vt)
    R = U @ np.diag([1, 1, d]) @ Vt
    return R  # maps sensor frame → body frame
```

Reject the fit and warn the user if RMS angular residual across all poses exceeds 5 deg.

### 2d. Apply R_sb in the Heading Pipeline

In `_compute_heading`, after applying hard/soft iron (which are now in ICM-20948 frame),
apply R_sb before tilt compensation:

```python
mag_body  = R_sb @ mag_c          # ICM-20948 frame → vehicle body frame
accel_body = R_sb @ accel_raw     # same R_sb, same source frame — now valid

roll  = atan2(accel_body[1], accel_body[2])
pitch = atan2(-accel_body[0], sqrt(accel_body[1]**2 + accel_body[2]**2))

mx_h = mag_body[0]*cos(pitch) + mag_body[2]*sin(pitch)
my_h = (mag_body[0]*sin(roll)*sin(pitch)
        + mag_body[1]*cos(roll)
        - mag_body[2]*sin(roll)*cos(pitch))

heading_mag = degrees(atan2(-my_h, mx_h)) % 360.0
```

When `sensor_to_body` is absent in the stored calibration (legacy entries), R_sb
defaults to identity and behavior is identical to Step 1 alone.

---

## Step 3 — Replace the Declination Stub

`_magnetic_declination` in `session.py` always returns `0.0`. Replace with a real value.

Minimum acceptable fix: a hardcoded regional constant (e.g. `12.5` for Northern
California deployments). Add a `declination_deg` field to the deployment config so it
can be updated without code changes.

Full fix: implement a lightweight WMM lookup. The `pyIGRF` or `geomag` packages provide
point-in-time declination from GPS lat/lon with ~0.1 deg accuracy. GPS coordinates are
already available in STATUS packets (`lat_e7`, `lon_e7`) when `STATUS_GPS_VALID` is set.

---

## Validation Criteria

Run after completing all three steps and recalibrating at least one node:

1. **Yaw sweep on level surface** — rotate 4 cardinal yaw turns of ~90 deg each.  
   Accept: each step reports 90 ± 5 deg, full 360-deg closure error < 10 deg.

2. **Known absolute heading** — point node body axis toward a compass-verified bearing.  
   Accept: reported `heading_true_deg` within ± 5 deg of known bearing.

3. **Tilt robustness** — repeat yaw sweep with ~15 deg deliberate pitch and roll.  
   Accept: heading error does not increase by more than 3 deg compared to flat.

4. **Legacy calibration compatibility** — load a pre-Step-2 calibration file (no
   `sensor_to_body` field). Accept: system falls back to identity, logs
   `alignment=identity` once at startup, heading behaves as Step 1 alone.

---

## CLI Visibility

Calibration status output should include:

```
node 1  uid=0xA1B2C3D4  calibration=valid  alignment=present  declination=12.5deg
node 2  uid=0xB5C6D7E8  calibration=valid  alignment=identity (recalibration recommended)
```

---

## Summary of Changes Required

| Layer | Change |
|---|---|
| `session.py: _compute_heading` | Apply AK09916→ICM permutation before hard/soft iron (Step 1) |
| `session.py: _compute_heading` | Apply R_sb to both mag and accel before tilt compensation (Step 2d) |
| `session.py: on_calibration_data` | Store `sensor_to_body` field; default identity when absent |
| `session.py: _magnetic_declination` | Replace stub with real WMM lookup or hardcoded regional value (Step 3) |
| `session.py` | Add `fit_sensor_to_body(observations)` utility |
| `session.json` schema | Add optional `sensor_to_body` per calibration entry |
| CLI | Show `alignment=present\|identity` and `declination=N deg` in calibration status |
| All nodes | Recalibrate after Step 1 deployment (existing calibration is in AK09916 frame) |
