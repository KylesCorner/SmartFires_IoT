# DMP Cleanup Plan

## Background

The ICM-20948 DMP 9DOF Rotation Vector is now working correctly. It computes heading
on-chip (gyro + accel + mag fusion) and transmits it via the STATUS packet. The
Jetson-side calibration pipeline (Welford stats → eigendecomposition → soft-iron
correction → tilt compensation) is no longer needed.

This plan removes all raw IMU transport and calibration infrastructure, replacing it
with a clean 2-field heading transport in the STATUS packet.

---

## New StatusPayload Wire Format

**Before (24 bytes):**
```
lat_e7(i32) lon_e7(i32) battery_mv(u16) battery_pct(u8) flags(u8)
mag_x(i16) mag_y(i16) mag_z(i16) accel_x(i16) accel_y(i16) accel_z(i16)
```

**After (16 bytes):**
```
lat_e7(i32) lon_e7(i32) battery_mv(u16) battery_pct(u8) flags(u8)
heading_deg_x10(u16) heading_accuracy(u16)
```

`heading_deg_x10`: heading × 10, range 0–3590, valid when `STATUS_IMU_VALID` set.
`heading_accuracy`: Q12 raw; divide by 4096 for degrees.

Total STATUS LoRa packet: 4 (header) + 16 (payload) + 1 (CRC) = **21 bytes** (was 29).

---

## Phase 1 — Wire Format: `BinaryPacket.h`

- Replace 6 raw IMU int16 fields in `StatusPayload` with `heading_deg_x10` (u16) + `heading_accuracy` (u16)
- Remove `STATUS_IMU_DMP = 0x08` flag (DMP is the only mode; `STATUS_IMU_VALID` alone is sufficient)
- Update `static_assert` for `StatusPayload`: 24 → 16
- Update file header comment: STATUS size 29 → 21 bytes
- Remove `CalibrationDataPayload` struct
- Remove `encodeCalibrationDataPayload()` and `decodeCalibrationData()`
- Remove `kCalibrationDataLoRaSize` constant
- Remove `CALIB_DATA` line from file header comment
- Remove `static_assert` for `CalibrationDataPayload`
- Keep: `CmdCalibratePayload`, `CmdResetPayload`, `CmdAckPayload` and their encoders/decoders

## Phase 2 — Internal Data Model

**`SensorSnapshot.h`**
- Remove: `magX`, `magY`, `magZ`, `accelX`, `accelY`, `accelZ`, `imuDmp`
- Add: `float headingDeg = -1.0f`, `uint16_t headingAccuracy = 0`
- Keep: `bool imuValid = false`

**`IIcm20948Driver.h` `Data` struct**
- Remove: `accelX/Y/Z`, `gyroX/Y/Z`, `magX/Y/Z` (SparkFun driver no longer populates them)
- Keep: `headingDeg`, `headingAccuracy`, `headingValid`, `valid`

## Phase 3 — Firmware Sensor + Packet Assembly

**`Icm20948Sensor.cpp`**
- `fillSnapshot()`: remove raw/DMP branch structure; always write heading fields
- `sample()`: remove else-branch raw log (dead code)
- `writeTelemetry()`: replace accel/gyro fields with `heading_deg=` and `accuracy_deg=`
- Remove `clampToInt16()` file-local helper

**`PacketHandler.cpp`**
- `tryEncodeStatus()`: replace raw IMU block with heading fields; remove `STATUS_IMU_DMP` reference
- Update `status_imu_payload` debug log

## Phase 4 — Python Packet Layer: `packet.py`

- `STATUS_PAYLOAD_FMT`: `"<iiHBBhhhhhh"` → `"<iiHBBHH"`
- Update `STATUS_PAYLOAD_SIZE` (24 → 16), `STATUS_LORA_SIZE` (29 → 21)
- Remove `STATUS_IMU_DMP = 0x08`
- `decode_status()`: replace 6 raw IMU fields with `heading_deg` (raw/10.0) + `heading_accuracy` (raw)
- `decode_gps()`: update unpack to match new struct
- Remove: `CALIBRATION_DATA_PAYLOAD_FMT/SIZE`, `CALIBRATION_DATA_LORA_SIZE`, `decode_calibration_data()`

## Phase 5 — Python Session: `session.py`

Remove entirely:
- `on_calibration_data()` — Welford → eigendecomposition pipeline
- `fit_sensor_to_body()` — Wahba/Kabsch alignment
- `set_alignment()` — no alignment state
- `clear_calibration_by_node()` + `clear_calibrations()`
- `_compute_heading()` — replaced by direct field read
- `_magnetic_declination()` — only called from `_compute_heading()`
- `"calibrations"` key from `_default_state()` and all calibration serialization in `load()` / `save()`
- `import numpy as np` + `import math` (no longer needed)
- `has_calibration` from `on_awaken()` return dict

Simplify `on_status()` — no calibration lookup; read heading directly from decoded packet:
```python
if status.get("imu_valid") and status.get("heading_deg") is not None:
    node_status["heading_true_deg"] = status["heading_deg"]
    node_status["heading_accuracy_deg"] = round(status.get("heading_accuracy", 0) / 4096.0, 2)
    node_status["last_heading_ts"] = int(time.time())
    return {"computed": True, "heading_true_deg": status["heading_deg"]}
return {"computed": False}
```

## Phase 6 — Ingest Service: `ingest_service.py`

- Remove the `PKT_CALIBRATION_DATA` handler (branch that calls `session.on_calibration_data()`)
- The packet type value remains in the enum but the ingest service logs-and-drops it

## Phase 7 — Documentation

- Delete `documentation/SENSOR_TO_BODY_ALIGNMENT_PLAN.md`
- Rewrite `documentation/Heading_CLI_Development/ORIENTATION_CALIBRATION_PLAN.md` — replace
  Welford/eigendecomposition workflow with a description of DMP self-calibration
- Update `CLAUDE.md` and `SOFTWARE_DESIGN.md` STATUS packet size and field tables

---

## What Is NOT Changing

| Item | Reason |
|---|---|
| `PKT_CMD_CALIBRATE / CMD_RESET` packet types | Future use: trigger DMP bias reset or node reset from CLI |
| CLI heading display | Reads `node_status["heading_true_deg"]` — no change needed |
| `on_cmd_ack()` in session.py | Command acknowledgement from node |
| AWAKEN / BUNDLE / FULL_STATE packets | Unrelated to IMU |
