---
name: duty-cycling
description: DutyCycleController's wake/sample/sleep state machine and its config/trigger sensor.
category: architecture
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/platformio.ini
  - platformio/include/config/NetworkProfiles.h
  - platformio/include/power/DutyCycleController.h
  - platformio/src/power/DutyCycleController.cpp
  - platformio/include/config/SensingConfig.h
related_docs:
  - tunable-parameters
---

# Duty cycling

`DutyCycleController` owns sensor wake, warmup, sampling, and sleep phases. `SmartFiresNodeApp` advances it on each loop, consumes `telemetryReady()`, converts sensor state into `SensorSnapshot`, and then hands the snapshot to `PacketHandler`.

## Modes

| Mode | Wake condition | Sample period | Warmup | Active window | Cycle/standby |
|---|---|---:|---:|---:|---|
| Continuous | Never sleeps intentionally | SF7/9 750 ms; SF10 1 s; SF12 4 s | 10 s once | Unbounded | None |
| SensorTriggered | SHT31 threshold after minimum sleep | Fixed 750 ms | 10 s | 30 s | Minimum 3 s sleep |
| Timed | RTC deadline | SF7/9/10 1 s; SF12 4 s | 10 s | SF7/9/10 30 s; SF12 64 s | 75/80/90/150 s wake-to-wake, minimum 5 s standby |
| Hybrid | Trigger or timer | Fixed 750 ms | 10 s | 30 s | 5 min + warmup + active; minimum 5 s standby |

Build selection is `SMARTFIRES_DUTY_CYCLE_MODE`: 0 Continuous, 1 SensorTriggered, 2 Timed, 3 Hybrid. The deployment target `feather_m0_lora_node` is Continuous; `node_debug` and `node_timed` are Timed; `node_hybrid` remains available. `SMARTFIRES_NETWORK_PROFILE` supplies the Continuous/Timed cadence, operational bundle size, final-drain, and Timed-cycle values for SF7/SF9/SF10/SF12. SensorTriggered and Hybrid retain fixed legacy cadence and are not supported deployment combinations for higher-SF trials.

## State flow

```text
startup / wake
    -> SensorWarmup
    -> ActiveSampling
    -> SensorCooldown
    -> Sleeping
    -> SensorWarmup ...
```

Continuous remains in `ActiveSampling` after its first warmup. Other modes call each sensor's wake/sleep behavior according to its `SensorDutyClass`. Sampling is globally scheduled by the active profile but each sensor also enforces its own minimum period.

## SensorTriggered

The trigger source is SHT31 temperature/humidity. After at least 3 seconds asleep, the controller polls for up to 1 second per update and wakes when either absolute delta from the reference reaches:

- 1.0 °C
- 5.0 percentage points relative humidity

There is no timer wake in this mode. Once triggered, sensors warm for 10 seconds, sample for 30 seconds at the fixed 750 ms cadence, cool down, and return to trigger monitoring. MCU standby is not entered by the current node application for SensorTriggered mode; sensor/radio power behavior is separate from the Timed MCU-standby path.

## Timed

Timed mode is deliberately aligned to two operational packet boundaries. For SF7/SF9/SF10:

```text
2 bundles * 15 samples/bundle * 1,000 ms = 30,000 ms active
```

SF12 instead uses `2 bundles * 8 samples/bundle * 4,000 ms = 64,000 ms`. The normal window emits two full bundles with no partial-bundle flush. If blocking sensor work starves sampling, the controller may hold the window open for up to one operational bundle period. On that ceiling it permits the application to force-encode the partial bundle instead of losing accumulated samples.

The selected 75/80/90/150-second wake-to-wake target is divided into time already spent in warmup, active sampling/overrun, post-window TX drain, and the remaining standby. Standby never falls below 5 s; if work runs longer, the cycle stretches.

`WINDOW_BEGIN` is queued after wake and `WINDOW_END` at close. The end marker carries `planned_sleep_ms` and the number of samples in the completed window. The application keeps the radio awake for up to the selected 5/8/15/30-second final-drain limit while draining telemetry and the marker, then enters SAMD21 RTC standby when the remaining duration is at least 250 ms.

The RTC COUNT32 implementation keeps subsecond time and supports continuing TDMA session time across standby. The watchdog is currently disabled for the actual standby interval; extending coverage is a documented, deferred possibility.

## Hybrid and SensorTriggered status

Hybrid combines the SensorTriggered thresholds with a scheduled wake. Its nominal cycle is 5 minutes plus the 10-second warmup and 30-second active period. A qualifying trigger may shorten sleep after the three-second minimum. Hybrid and SensorTriggered remain compiled modes, but neither is the production node target; they retain a fixed 750 ms cadence and their 30-second windows are not reshaped to whole higher-SF bundles.

## Sensor timing classes

| Sensor | Minimum sample period | Wake delay | Duty class |
|---|---:|---:|---|
| SHT31 | 100 ms | none | AlwaysOn |
| Wind Rev C | 10 ms | 10 s | WarmupHeavy |
| SPS30 | 1,000 ms | 8 s | WarmupHeavy |
| ICM-20948 | 10 ms | none | DutyCycled |
| PA1010D GPS continuous | 100 ms | none | GPS-specific modes |

The controller's 10-second warmup covers the slowest normal sensor wake. GPS also exposes periodic and AlwaysLocate power profiles in `SensingConfig.h`; those are distinct sensor-driver choices, not controller modes.

## Sample errors and telemetry

All controller modes set `failOnSampleError=false`. A failed sensor sample does not abort the entire controller cycle; validity is represented by snapshot flags and downstream packet fields. STATUS emission is handled by `PacketHandler`, not by the controller; its selected interval is 15/30/120/300 seconds.

Changing a profile requires checking bundle alignment, warmup-heavy sensors, radio drain time, RTC standby, watchdog behavior, and the base's sleep-aware ACK deferral together.
