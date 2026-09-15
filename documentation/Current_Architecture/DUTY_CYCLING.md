---
name: duty-cycling
description: DutyCycleController's wake/sample/sleep state machine and its config/trigger sensor.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
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
| Continuous | Never sleeps intentionally | 750 ms | 10 s once | Unbounded | None |
| SensorTriggered | SHT31 threshold after minimum sleep | 750 ms | 10 s | 30 s | Minimum 3 s sleep |
| Timed | RTC deadline | 1,000 ms | 10 s | 30 s, up to 15 s overrun | 75 s wake-to-wake, minimum 5 s standby |
| Hybrid | Trigger or timer | 750 ms | 10 s | 30 s | 5 min + warmup + active; minimum 5 s standby |

Build selection is `SMARTFIRES_DUTY_CYCLE_MODE`: 0 Continuous, 1 SensorTriggered, 2 Timed, 3 Hybrid. Current environments select SensorTriggered for `feather_m0_lora_node`, Timed for `node_debug` and `node_timed`, and Hybrid for `node_hybrid`.

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

There is no timer wake in this mode. Once triggered, sensors warm for 10 seconds, sample for 30 seconds at 750 ms, cool down, and return to trigger monitoring. MCU standby is not entered by the current node application for SensorTriggered mode; sensor/radio power behavior is separate from the Timed MCU-standby path.

## Timed

Timed mode is deliberately aligned to packet boundaries:

```text
2 bundles * 15 samples/bundle * 1,000 ms = 30,000 ms active
```

The normal window therefore emits 30 samples as two full bundles, with no partial-bundle flush. If blocking sensor work starves sampling, the controller may hold the window open for up to one bundle period (15 seconds). On that ceiling it permits the application to force-encode the partial bundle instead of losing accumulated samples.

The fixed 75-second wake-to-wake target is divided into time already spent in warmup, active sampling/overrun, post-window TX drain, and the remaining standby. Nominally this is 10 s warmup + 30 s active + about 35 s standby. Standby never falls below 5 s; if work runs longer, the cycle stretches.

`WINDOW_BEGIN` is queued after wake and `WINDOW_END` at close. The end marker carries `planned_sleep_ms` and the number of samples in the completed window. The application keeps the radio awake for up to 5 seconds while draining the final telemetry and marker, then enters SAMD21 RTC standby when the remaining duration is at least 250 ms.

The RTC COUNT32 implementation keeps subsecond time and supports continuing TDMA session time across standby. The watchdog is currently disabled for the actual standby interval; extending coverage is a documented, deferred possibility.

## Hybrid

Hybrid combines the SensorTriggered thresholds with a scheduled wake. Its nominal cycle is 5 minutes plus the 10-second warmup and 30-second active period. A qualifying trigger may shorten sleep after the three-second minimum. The 750 ms sample period does not divide a bundle cleanly at the 30-second boundary, so Hybrid does not enable Timed's full-bundle overrun behavior.

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

All active profiles currently set `failOnSampleError=false`. A failed sensor sample does not abort the entire controller cycle; validity is represented by snapshot flags and downstream packet fields. STATUS emission is handled by `PacketHandler`, not by duty-cycle timing, and current node environments set its interval to 15 seconds.

Changing a profile requires checking bundle alignment, warmup-heavy sensors, radio drain time, RTC standby, watchdog behavior, and the base's sleep-aware ACK deferral together.
