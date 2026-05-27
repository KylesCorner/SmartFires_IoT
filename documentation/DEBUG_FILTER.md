# SmartFires Debug Monitor

The SmartFires node debug build prints structured debug logs over USB serial.

Instead of using a plain serial monitor, we use a PlatformIO monitor filter that formats the logs and lets us filter by stream.

Each log line has a source stream such as:

- base
- app
- boot
- i2c
- battery
- gps
- imu
- sht31
- sps30
- wind
- tdma
- radio
- packet
- duty

### Log Levels
| Level | Meaning |
| ----- | ------- |
| T     | Trace   |
| D     | Debug   |
| I     | Info    |
| W     | Warning |
| E     | Error   |

---

## Start the debug monitor

From the PlatformIO project folder:

```bash
cd ~/SmartFires/code/platformio
pio device monitor -e feather_m0_lora_node_debug
```

You can apply filters by the example below showing a tdma filter with a min level of warning.

```bash
SFDBG_SRC=tdma SFDBG_MIN_LEVEL=W pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=radio,tdma,calibration  pio device monitor -e feather_m0_lora_node_debug
```

You can also apply multiple filter streams

```bash
SFDBG_SRC=tdma,i2c,gps SFDBG_MIN_LEVEL=W pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=calib pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=IMU pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=packet SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=packet pio device monitor -e feather_m0_lora_node_debug
```

To watch the exact raw IMU values packed into each STATUS payload, use the
`packet` stream at debug level. Look for `status_imu_payload`, which prints
`mag_xyz` in `uT x 10` and `accel_xyz` in `mg`:

```bash
SFDBG_SRC=packet SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug
```

For a dedicated IMU troubleshooting stream that is independent of STATUS timing,
also watch `imu_raw_sample` on the `IMU` stream. This emits on each
successful IMU sample and prints raw magnetometer (`uT`), accelerometer (`mg`),
and gyro (`dps`) values:

```bash
SFDBG_SRC=IMU SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug
```

Base station now uses the same structured logger. To follow communication-layer
traffic on the base station:

```bash
SFDBG_SRC=base,app,calibration SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_base
SFDBG_SRC=base,app,calibration pio device monitor -e feather_m0_lora_base
```

## Plan: #2 Sensor-to-Body Alignment Matrix

Goal: correct persistent yaw under/over-rotation caused by axis misalignment between IMU sensor frame and the vehicle/body frame.

### 1. Add alignment parameters to persisted calibration

- Extend receiver calibration state with a 3x3 `sensor_to_body` rotation matrix.
- Default to identity matrix for backward compatibility with existing calibration files.
- Store matrix alongside `hard_iron` and `soft_iron` per `uid_hash`.

### 2. Collect alignment dataset

- Capture at least 6 static poses with known body orientation (front, back, left, right, nose-up, nose-down).
- For each pose, collect averaged accelerometer and magnetometer vectors after hard/soft-iron correction.
- Save pose-tagged samples for repeatable re-fit without reflashing nodes.

### 3. Solve alignment rotation

- Use Wahba/Kabsch-style least-squares fit to compute rotation from sensor-frame vectors to body-frame vectors.
- Enforce proper rotation constraints (`R^T R = I`, `det(R)=+1`) to avoid reflection artifacts.
- Reject fit if residual error exceeds threshold (for example RMS > 5 deg equivalent).

### 4. Apply alignment in heading pipeline

- In heading compute path, transform corrected vectors with alignment matrix before tilt compensation:
	- `mag_body = R_sb @ mag_corrected`
	- `accel_body = R_sb @ accel_raw`
- Keep existing tilt compensation equations, now fed with body-frame vectors.

### 5. Validation criteria

- Bench test 4 cardinal yaw turns (about 90 deg each) on level surface.
- Acceptance: each step reports 90 deg +/- 5 deg and full turn closure error < 10 deg.
- Repeat with mild pitch/roll to ensure tilt compensation remains stable.

### 6. Rollout and fallback

- If no alignment matrix exists, continue identity behavior and log `alignment=identity` once at startup.
- Add CLI visibility: calibration status should show `alignment=present|identity`.
