---
name: tunable-parameters
description: Every tunable constant in the system — TDMA, sensing/duty-cycle, power, Jetson.
category: architecture
status: current
last_verified: 2026-08-17
source_refs:
  - platformio/include/config/NetworkConfig.h
  - platformio/include/config/SensingConfig.h
  - platformio/include/config/PowerConfig.h
  - platformio/include/config/BaseConfig.h
related_docs:
  - tdma-protocol
  - packet-reliability
  - duty-cycling
---

# SmartFires — Tunable Parameters

Single reference for every configurable value in the system.  After the
tunable-parameter consolidation (see `documentation/Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md`),
each constant lives in exactly one place; all consuming code imports it from
there.  Change a value in its source header/module and it propagates everywhere
automatically.

---

## Network and TDMA

**Source (firmware):** `platformio/include/config/NetworkConfig.h` — `NetworkConfig` namespace

| Constant | Value | Meaning |
|---|---|---|
| `kNumSlots` | 4 (from `NUM_SLOTS` build flag) | TDMA slots per frame; must match across all node Feathers |
| `kSlotWidthMs` | 900 ms | Slot duration; fits worst-case one bundle TX (340 ms) + one link-ACK timeout (250 ms) + 2× guard |
| `kGuardMs` | 20 ms | Guard time at slot edges; covers ≤50 ppm crystal drift at 22 min max sync interval |
| `kRxWakeAheadMs` | 150 ms | How long before slot 0 a node starts waking its radio for Rx gating (`TdmaClock::baseRxWindowOpen()`); distinct from `kGuardMs` — also covers main-loop jitter from blocking sensor reads, not just crystal drift. Starting value, not yet bench-characterized |
| `kSyncStaleMs` | 1 320 000 ms (22 min) | Node transmits unconditionally after this long without a TIME_SYNC |
| `kBaseAddr` | 0x01 | RadioHead address of the base station |
| `kRadioFrequencyMhz` | 915.0 MHz | LoRa carrier frequency |
| `kRadioTxPowerDbm` | 13 dBm | RadioHead TX power |
| `kLinkRetries` | 3 | RadioHead `RHReliableDatagram` retransmit attempts per send |
| `kLinkAckTimeoutMs` | 250 ms | Per-attempt link-layer ACK timeout |
| `kQueueDepth` | 8 | `TdmaTxQueue` capacity (drop-oldest when full) |
| `kQueueCapacityHardCap` | 8 | Compile-time upper bound enforced by static_assert |
| `kReliabilityWindowDepth` | 8 | App-layer pending-retry window size |
| `kReliabilityWindowHardCap` | 8 | Compile-time upper bound enforced by static_assert |
| `kReliabilityMaxAttempts` | 3 | Max app-layer retransmit attempts per packet |
| `kReliabilityMaxAgeMs` | 30 000 ms | Evict pending packet from window after this age |
| `kReliabilityMode` | `AppLayerAckSummary` | Active reliability mode (build-flag selectable via `SMARTFIRES_TDMA_RELIABILITY_MODE`) |
| `kExpectedAckIntervalMs` | 4 000 ms | ACK-paced retry gate: expected time between ACK_SUMMARY packets |
| `kRetryWaitMultiplierPermille` | 2000 (×2.0) | ACK-paced retry gate: back-off multiplier in permille |
| `kRetryWaitMinMs` | 4 500 ms | Minimum retry wait |
| `kRetryWaitMaxMs` | 10 000 ms | Maximum retry wait |
| `kRequireAckSummaryBeforeFirstRetry` | false | Whether to wait for an ACK_SUMMARY before the first retransmit |
| `kAwakenIntervalMs` | 5 000 ms | Re-broadcast PKT_AWAKEN every N ms while waiting for TIME_SYNC |
| `kStatusIntervalMs` | 900 000 ms (15 min) | How often PKT_STATUS is emitted (overridable via `SMARTFIRES_STATUS_INTERVAL_MS`) |
| `kEnableTelemetryTx` | true | Set false to suppress BUNDLE TX (STATUS still flows); flip for normal operation |
| `kBundleTxBudgetMs` | 340 ms | Estimated on-air time for a PKT_BUNDLE (used for slot budget check) |
| `kStatusTxBudgetMs` | 120 ms | Estimated on-air time for a PKT_STATUS |
| `kAwakenTxBudgetMs` | 90 ms | Estimated on-air time for a PKT_AWAKEN |
| `kDefaultTxBudgetMs` | 140 ms | Fallback estimate for unknown packet types |

---

## Base Station Bridge

**Source (firmware):** `platformio/include/config/BaseConfig.h` — `BaseConfig` namespace

| Constant | Value | Meaning |
|---|---|---|
| `kTdmaNumSlots` | = `NetworkConfig::kNumSlots` | Mirrors node's NUM_SLOTS — both must agree for ACK-summary slot-window math |
| `kTdmaSlotWidthMs` | = `NetworkConfig::kSlotWidthMs` | Mirrors node's slot width |
| `kTdmaGuardMs` | = `NetworkConfig::kGuardMs` | Mirrors node's guard time |
| `kTotalEntities` | = `NetworkConfig::kNumSlots` | Size of base's node-assignment table (1 base + N nodes → N slots) |
| `kMaxAssignedNodes` | `kTotalEntities − 1` | Derives automatically; base is entity 0 |
| `kFirstNodeId` | 0x02 | Lowest assignable node ID (0x01 = base, 0x00 = unassigned) |
| `kMaxAckTrackedNodes` | 16 | Upper bound for ACK-summary bitmap tracking table |
| `kPeriodicTimeSyncMs` | 50 000 ms | Base firmware fallback TIME_SYNC interval (Jetson normally sends every 600 s) |
| `kHealthLogPeriodMs` | 5 000 ms | Periodic health-log print interval in base firmware |

---

## Sensing — Duty Cycle

**Source (firmware):** `platformio/include/config/SensingConfig.h` — `SensingConfig::DutyCycle` namespace

Four named constant sets exist in `SensingConfig::DutyCycle`, one per controller
mode. There are no `dutyCycleCfgContinuous()` / `dutyCycleCfg()` factory
functions — the only runtime factory is `DutyCycleConfig::make(...)`
(`include/power/DutyCycleController.h`). `main.cpp` wires it up exclusively from
a derived `kActive*` set, which `SensingConfig.h` resolves from the
`SMARTFIRES_DUTY_CYCLE_MODE` build flag (set per-environment in
`platformio.ini`). An unrecognised value is a `#error`, not a silent default.

| `SMARTFIRES_DUTY_CYCLE_MODE` | `DutyCycleMode` | Active set | Environment |
|---|---|---|---|
| `0` | `Continuous` | `kContinuous*` (`enabled = false`) | — |
| `1` (default if unset) | `SensorTriggered` | `kSensorTriggered*` | `feather_m0_lora_node` |
| `2` | `Timed` | `kTimed*` | `feather_m0_lora_node_debug`, `feather_m0_lora_node_timed` |
| `3` | `Hybrid` | `kHybrid*` | `feather_m0_lora_node_hybrid` — deprecated |

### Cross-mode constants

| Constant | Value | Meaning |
|---|---|---|
| `kMinMcuStandbyMs` | 250 ms | Shortest remaining sleep worth entering MCU standby for; below this the enter/exit overhead isn't worth it |
| `kMaxTxDrainBeforeStandbyMs` | 5 000 ms | How long the node stays awake at the end of an active window waiting for the TX queue to drain before entering standby anyway (≈ one TDMA frame plus slack) |

### Sensor-triggered constant set (real node build)

| Constant | Value | Meaning |
|---|---|---|
| `kSensorTriggeredMode` | `SensorTriggered` | Wakes only on a threshold crossing — no scheduled timer wakeup |
| `kSensorTriggeredMinSleepMs` | 3 000 ms | Minimum idle sleep before a trigger may wake |
| `kSensorTriggeredMaxWakeMs` | 1 000 ms | Max additional wake delay |
| `kSensorTriggeredActiveSampleMs` | 30 000 ms | Duration of the `ActiveSampling` window |
| `kSensorTriggeredSamplePeriodMs` | 750 ms | Sample cadence within `ActiveSampling` |
| `kSensorTriggeredWarmupMs` | 10 000 ms | Sensor stabilization delay after wake |
| `kSensorTriggeredTimedSleepMs` | 0 ms | Ignored in this mode |
| `kSensorTriggeredTempDeltaThresholdC` | 1.0 °C | Temperature delta to trigger early wake |
| `kSensorTriggeredHumidityDeltaThresholdPct` | 5.0 %RH | Humidity delta to trigger early wake |
| `kSensorTriggeredFailOnSampleError` | false | Sensor errors do not halt the node |

### Timed constant set (scheduled wake — the only mode that enters MCU standby)

| Constant | Value | Meaning |
|---|---|---|
| `kTimedMode` | `Timed` | Fixed sleep/active cycle; trigger thresholds ignored |
| `kTimedTimedSleepMs` | 35 000 ms | Sleep between active windows — the MCU standby duration |
| `kTimedActiveSampleMs` | 25 000 ms | Duration of the `ActiveSampling` window |
| `kTimedSamplePeriodMs` | 1 000 ms | Sample cadence within `ActiveSampling` |
| `kTimedWarmupMs` | 10 000 ms | Sensor stabilization delay after each wake |
| `kTimedMinSleepMs` | 0 ms | `CooldownSleeping` hands straight over to `IdleSleeping` |
| `kTimedMaxWakeMs` | 1 000 ms | Max additional wake delay |
| `kTimedTempDeltaThresholdC` / `kTimedHumidityDeltaThresholdPct` | 0.0 | Ignored in this mode |
| `kTimedFailOnSampleError` | false | Sensor errors do not halt the node |

At these values one window produces 25 samples — one full 15-sample bundle plus a
10-sample partial that is force-flushed at window close (see
[DUTY_CYCLING.md](DUTY_CYCLING.md#active-windows-on-the-wire-timed-mode)).

### Continuous constant set (duty-cycle gate disabled)

| Constant | Value | Meaning |
|---|---|---|
| `kContinuousMode` | `Continuous` | Duty-cycle gate is disabled — sensors run back-to-back at `samplePeriodMs` |
| `kContinuousMinSleepMs` | 0 ms | Not used in continuous mode |
| `kContinuousMaxWakeMs` | 0 ms | Not used in continuous mode |
| `kContinuousActiveSampleMs` | 0 ms | Not used in continuous mode |
| `kContinuousSamplePeriodMs` | 750 ms | Master loop cadence — how often the sensor-service tick fires |
| `kContinuousWarmupMs` | 10 000 ms | One-time warmup delay at boot before first sample |
| `kContinuousTimedSleepMs` | 0 ms | Not used in continuous mode |
| `kContinuousTempDeltaThresholdC` | 0.0 °C | Not used in continuous mode |
| `kContinuousHumidityDeltaThresholdPct` | 0.0 %RH | Not used in continuous mode |
| `kContinuousFailOnSampleError` | false | Sensor errors do not halt the node |

### Hybrid constant set (deprecated)

Wakes on either a threshold crossing after `kHybridMinSleepMs` (3 000 ms) or
`kHybridTimedSleepMs` (5 min), whichever comes first; `kHybridActiveSampleMs`
30 000 ms, `kHybridSamplePeriodMs` 750 ms, `kHybridWarmupMs` 10 000 ms. Retained
so the `feather_m0_lora_node_hybrid` env still compiles, but not a target for
further work — `Hybrid` never enters MCU standby.

---

## Sensing — Per-Sensor

Each sensor has its own independently-tuned namespace in `SensingConfig.h`.
Values are NOT generalized across sensors (e.g., Wind and SPS30 happen to share
some timing coincidentally; they are still listed separately so each can be
changed independently without affecting the other).

**Source (firmware):** `platformio/include/config/SensingConfig.h`

### SHT31 — Temperature / Humidity (trigger sensor)

Namespace `SensingConfig::Sht31`

| Constant | Value | Meaning |
|---|---|---|
| `kMinSamplePeriodMs` | 100 ms | Minimum interval between samples |
| `kDutyClass` | `AlwaysOn` | Always powered — used as the trigger sensor for ThresholdTriggered duty cycle |

I²C address (0x45) is not a `SensingConfig.h` constant — it is a default
constructor parameter on `Sht31Sensor` (`include/sensors/Sht31Sensor.h`),
forwarded to `AdafruitSht31Driver::begin()`.

### Wind — RevC Hot-Wire Anemometer

Namespace `SensingConfig::Wind`

| Constant | Value | Meaning |
|---|---|---|
| `kDividerRatio` | 1.6818 | Voltage divider ratio applied to both RV and TMP ADC channels |
| `kZeroWindAdjustmentVolts` | −1.0 V | Calibration offset: voltage at zero wind speed |
| `kMinSamplePeriodMs` | 10 ms | Sample floor (ADC read is fast) |
| `kWakeDelayMs` | 10 000 ms | Hot-wire / TPS settling time after power-on |
| `kDutyClass` | `DutyCycled` | Powered by TPSDriver; woken and slept by DutyCycleController |

### SPS30 — Particulate Matter (PM1.0 / PM2.5 / PM4.0 / PM10)

Namespace `SensingConfig::Sps30`

| Constant | Value | Meaning |
|---|---|---|
| `kMinSamplePeriodMs` | 1 000 ms | Sensor fan spin-up produces one reading per second |
| `kWakeDelayMs` | 8 000 ms | Fan/laser warmup time after power-on |
| `kDutyClass` | `WarmupHeavy` | Managed by DutyCycleController warm-up state |

### ICM-20948 — IMU / DMP Heading

Namespace `SensingConfig::Imu`

| Constant | Value | Meaning |
|---|---|---|
| `kMinSamplePeriodMs` | 10 ms | DMP output rate |
| `kWakeDelayMs` | 0 ms | No additional settling beyond driver init |
| `kDutyClass` | `DutyCycled` | Follows duty-cycle controller |

`ad0Val` (AD0 pin state passed to `IIcm20948Driver::begin()`, selecting the
0x69 I²C address) is not a `SensingConfig.h` constant — it is supplied at the
`begin()` call site, not centralized here.

### PA1010D GPS

Namespace `SensingConfig::Gps` — three independently-tuned timing groups (Continuous,
Periodic Standby/Backup, AlwaysLocate Standby/Backup).

| Constant | Value | Mode(s) | Meaning |
|---|---|---|---|
| `kContinuousMinSamplePeriodMs` | 100 ms | Continuous | Sample floor for full-power continuous mode |
| `kContinuousWakeDelayMs` | 0 ms | Continuous | No delay |
| `kPeriodicRunTimeMs` | 24 000 ms | Periodic Standby/Backup | Active GPS window per cycle |
| `kPeriodicSleepTimeMs` | 90 000 ms | Periodic Standby/Backup | First sleep window |
| `kPeriodicSecondRunTimeMs` | 24 000 ms | Periodic Standby/Backup | Second active window per cycle |
| `kPeriodicSecondSleepTimeMs` | 90 000 ms | Periodic Standby/Backup | Second sleep window |
| `kPeriodicMinSamplePeriodMs` | 1 000 ms | Periodic Standby/Backup | Sample floor while active |
| `kAlwaysLocateMinSamplePeriodMs` | 1 000 ms | AlwaysLocate Standby/Backup | Sample floor |

I²C address (0x10) is not a `SensingConfig.h` constant — it is a default
constructor parameter on `Pa1010dGpsSensor` (`include/sensors/Pa1010dGpsSensor.h`),
forwarded to `AdafruitGpsDriver::begin()`.

---

## Power — Battery Monitor

**Source (firmware):** `platformio/include/config/PowerConfig.h` — `PowerConfig::Battery` namespace

| Constant | Value | Meaning |
|---|---|---|
| `kAdcRefVolts` | 3.3 V | ADC reference voltage (Feather M0 AREF) |
| `kAdcMax` | 1023 | ADC full-scale count (10-bit) |
| `kDividerRatio` | 2.0 | Voltage divider ratio on the battery sense pin |
| `kMinVoltage` | 3.2 V | Voltage reported as 0 % |
| `kMaxVoltage` | 4.2 V | Voltage reported as 100 % |
| `kLowVoltage` | 3.5 V | Threshold logged as low-battery warning |
| `kMinSamplePeriodMs` | 1 000 ms | Minimum interval between ADC reads |

---

## Edge Runtime (Jetson)

**Source (Python):** `edge/edge-receiver/src/smartfires_edge/config.py`

### Ingest / base-station link

| Constant | Default | CLI flag | Meaning |
|---|---|---|---|
| `DEFAULT_PORT` | `/dev/smartfires-base` | `--port` | Serial port for the base-station USB link (udev symlink — see UART_JETSON_BRIDGE.md) |
| `DEFAULT_BAUD` | 115200 | `--baud` | Baud rate passed to pyserial |
| `DEFAULT_DATA_DIR` | `/mnt/nvme_drive/data` | `--data-dir` | Root output directory |
| `DEFAULT_NODES` | `[1, 2]` | `--nodes` | Node IDs tracked for packet-loss metrics |
| `DEFAULT_METRICS_INTERVAL_S` | 10 s | `--metrics-interval` | Packet-loss state flush interval |
| `DEFAULT_SYNC_INTERVAL_S` | 600 s (10 min) | `--sync-interval` | Periodic TIME_SYNC broadcast interval |

### Anemometer (ES-W302, optional)

| Constant | Default | CLI flag | Meaning |
|---|---|---|---|
| `DEFAULT_ANEMOMETER_PORT` | `None` (disabled) | `--anemometer-port` | Serial port; omit to disable |
| `DEFAULT_ANEMOMETER_BAUD` | 9600 | `--anemometer-baud` | Modbus baud rate |
| `DEFAULT_ANEMOMETER_ADDRESS` | 1 | `--anemometer-address` | Modbus device address |
| `DEFAULT_ANEMOMETER_INTERVAL_S` | 1.0 s | `--anemometer-interval` | Modbus poll interval |

### Web dashboard

| Constant | Default | CLI flag | Meaning |
|---|---|---|---|
| `DEFAULT_WEB_HOST` | `0.0.0.0` | `--host` | FastAPI/uvicorn bind address |
| `DEFAULT_WEB_HTTP_PORT` | 8080 | `--http-port` | HTTP port |

### Visualizer

| Constant | Default | CLI flag | Meaning |
|---|---|---|---|
| `DEFAULT_TELEMETRY_ROWS` | 20 | `--telemetry-rows` | Max telemetry rows shown on screen |

### CLI (calibrate / reset commands)

| Constant | Value | Meaning |
|---|---|---|
| `CLI_CMD_ACK_TIMEOUT_S` | 5.0 s | Warn if no CMD_ACK received within this window |
| `CLI_CALIBRATION_DURATION_S` | 60 s | Duration sent in CMD_CALIBRATE frame |
