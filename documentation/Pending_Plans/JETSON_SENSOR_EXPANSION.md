# Jetson Sensor Expansion

## Background

Today the Jetson is purely a relay/ingest point — it receives node telemetry over the
base-station UART bridge and (optionally) polls one external Modbus anemometer
(`edge/edge-receiver/src/smartfires_edge/anemometer.py`). This plan adds sensors directly
attached to the Jetson itself (the base station's own local environment, position, and
orientation), independent of the node network:

1. Temperature/humidity sensor (I2C)
2. Anemometer — already done (ES-W302 via Modbus RS485)
3. BMV080 particulate matter sensor (I2C)
4. GPS (PA1010D, same part as the nodes) — base-station position, and a path to accurate
   time when deployed with no internet
5. ICM-20948 IMU — same on-chip DMP heading computation the nodes use

**Decided direction:** I2C for every new sensor except the anemometer (already wired via
Modbus and out of scope to change). Sensor processing happens in the Jetson's own process,
not offloaded to a secondary microcontroller.

No code has been written yet. This document captures the current plan and risk assessment
so implementation can pick up per-sensor.

---

## Existing patterns to reuse

- **`AnemometerPoller`** (`anemometer.py`) is the template for any new Jetson-local sensor:
  a background thread with a thread-safe `.latest()` getter, started/stopped in
  `ingest_service.run_receive`, config flags mirroring `AnemometerConfig` /
  `add_anemometer_args`, and dedicated `jetson_*`-prefixed columns in `csv_logger.py`
  (e.g. `jetson_wind_mps`, `jetson_wind_dir_deg`).
- **`BaseStationStore`** (`base_station_store.py`) already persists base-station `lat`/`lon`
  to `~/.smartfires/base_station.json`, currently populated manually via `web_service.py`'s
  web form. GPS is a drop-in automated source for this existing slot, not a new one.

---

## Status and risk per sensor

| Sensor | Status | Risk | Notes |
|---|---|---|---|
| Temp/Humidity | Not started | Low | Plain I2C register-read sensor, follow `AnemometerPoller` template directly |
| Anemometer (ES-W302) | **Done** | — | Already integrated via Modbus RS485 |
| BMV080 | Not started | **Research spike** | Bosch's chip needs a closed-source driver/firmware blob loaded at init — no existing foothold in this repo |
| GPS (PA1010D) | Not started | **Research spike** | Position is straightforward; offline time-sync path has an open implementation gap (see below) |
| ICM-20948 IMU (DMP heading) | Not started | Low–medium | Lower risk than first assumed — see below |

---

## Temp/Humidity

Pick an I2C temp/humidity part (e.g. SHT31, same family already used on the nodes), write a
poller module mirroring `AnemometerPoller`, add `jetson_temp_c` / `jetson_humidity_pct` CSV
columns and matching config flags. No open design questions.

---

## BMV080

Bosch's BMV080 does on-die optical particle-count processing and requires Bosch's
closed-source algorithm/firmware blob to be loaded into the chip at init — it is not a plain
register-read I2C sensor like the SPS30 the nodes use. Before scoping integration:

- Confirm Bosch's SDK has Linux ARM64 (Jetson/aarch64) bindings, or at minimum a C shared
  library that can be wrapped via ctypes/cffi.
- Confirm licensing/distribution terms for shipping that blob alongside `smartfires_edge`.

Once a working `read()` exists, it drops into the same poller pattern as the others, with
`jetson_pm1_0_ug` / `jetson_pm2_5_ug` / etc. columns.

---

## GPS (PA1010D)

Two separable concerns:

- **Position** — feeds the existing `BaseStationStore` (replacing manual lat/lon entry).
  Low rate, fits the poller pattern with no open questions once a read path exists.
- **Offline time discipline** — when deployed with no internet, GPS is the only time
  source. Decided approach: **`gpsd` + `chrony`** (GPS refclock) as independent systemd
  services, decoupled from `smartfires_edge`'s process lifecycle. Rationale: the system
  clock needs to stay correct even if the Python app isn't running, and disciplining the
  clock from inside the app would require elevated privileges (root / `CAP_SYS_TIME`) that
  don't fit the current user-level service model.
  - **Open implementation gap**: the PA1010D is I2C, not a serial device node, and `gpsd`
    expects a serial/USB/PTY source — it does not speak raw I2C. The usual fix (used in
    Adafruit's I2C GPS HAT writeups) is a small bridge process that reads NMEA sentences
    over I2C and feeds them into a PTY that `gpsd` treats as its serial source. This needs
    a short spike to confirm it works cleanly on the Jetson before committing further.
  - Once `gpsd` is running, the Python app reads position from gpsd's client socket
    rather than owning the I2C bus directly for GPS.

This also changes the existing design note in `CLAUDE.md` ("TIME_SYNC driven by Jetson NTP,
not GPS. GPS PPS sync deferred.") — once `chrony` is GPS-disciplined, the Jetson's own clock
becomes GPS-accurate even offline; the node TIME_SYNC broadcast mechanism itself is unaffected
since nodes still sync to the Jetson's session clock, but that session clock now has a path to
absolute-time accuracy without internet.

---

## ICM-20948 IMU / DMP heading

Initial assessment treated this as comparable in risk to BMV080 (closed vendor algorithm).
That was wrong, and is worth getting right because it changes the scoping significantly.

**Why it's not a vendor-SDK problem:** the node firmware's `SparkfunIcm20948Driver`
(`platformio/src/platform/SparkfunIcm20948Driver.cpp`) wraps a lower layer, `ICM_20948_C.c`,
vendored at `platformio/.pio/libdeps/*/SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino
Library/src/util/`. That C file is platform-agnostic — not Arduino-specific — and talks to
the chip through a two-function hardware vtable:

```c
typedef struct {
  ICM_20948_Status_e (*write)(uint8_t regaddr, uint8_t *pdata, uint32_t len, void *user);
  ICM_20948_Status_e (*read)(uint8_t regaddr, uint8_t *pdata, uint32_t len, void *user);
} ICM_20948_Serif_t;
```

The Arduino wrapper only supplies `Wire`-based implementations of those two functions. The
~14 KB DMP firmware image, the register-bank-switching protocol, FIFO frame parsing, and the
quaternion-to-heading math are all portable C already sitting in this repo's dependency tree
— there is no closed blob to source, unlike BMV080.

**Heading path** (mirrored from `SparkfunIcm20948Driver.cpp`):
1. `begin()` — I2C handshake, wake, bank select.
2. `initializeDMP()` — uploads the firmware image into chip memory over I2C; the chip's own
   DSP then runs gyro+accel+mag fusion on-chip.
3. `enableDMPSensor(ROTATION_VECTOR)`, `setDMPODRrate(...)`, `enableFIFO/DMP`,
   `resetDMP/FIFO` — register writes to enable fusion and set its output rate (5 Hz on the
   node firmware).
4. Per poll: `readDMPdataFromFIFO()` over I2C; if the frame carries Quat9 data, decode
   `Q1/Q2/Q3` (Q30 fixed-point), reconstruct `Q0` via the unit-quaternion identity, compute
   `yaw = atan2(2(q1q2+q0q3), q0²+q1²-q2²-q3²)`, normalize to 0–360°. Accuracy is a raw Q12
   value from the chip; negative means "not yet calibrated" — reject those frames.

**Port path**: implement `i2c_write`/`i2c_read` against Linux `i2c-dev`, plug them into the
same `ICM_20948_Serif_t`, and reuse `ICM_20948_C.c` plus the firmware image as-is (via
ctypes/cffi or a small native helper) rather than reimplementing any of the above. The
quaternion math itself is ~10 lines and trivially portable to Python if the lower layer is
exposed via a thin C extension. Same calibration caveat as the nodes applies: DMP biases are
RAM-only and lost on power cycle, so a figure-8 calibration motion is needed after every
reboot — for a base-station-mounted IMU this is a deployment-process question (who performs
it, and when) rather than a code question.

---

## Open questions / next steps

- [ ] BMV080: confirm Linux ARM64 SDK availability and licensing.
- [ ] GPS: spike the I2C→PTY NMEA bridge for `gpsd` on the Jetson.
- [ ] IMU: confirm `ICM_20948_C.c` compiles cleanly outside the Arduino toolchain (no hidden
  Arduino-only macros) and scope the i2c-dev `Serif` implementation.
- [ ] Decide deployment procedure for base-station IMU calibration (figure-8 motion after
  every Jetson/base power cycle).
- [ ] Data model: new `jetson_*` CSV columns per sensor, consistent with the existing
  anemometer convention.
