---
name: duty-cycling
description: DutyCycleController's wake/sample/sleep state machine and its config/trigger sensor.
category: architecture
status: current
last_verified: 2026-06-23
source_refs:
  - platformio/include/power/DutyCycleController.h
  - platformio/src/power/DutyCycleController.cpp
  - platformio/include/config/SensingConfig.h
related_docs:
  - tunable-parameters
---

# Duty Cycling

`DutyCycleController` owns the sensor wake/sample/sleep state machine for every
node. It gates when sensing is active, runs sensors through a warmup period,
decides when a telemetry snapshot is ready, and puts sensors back to sleep.

The application loop (`SmartFiresNodeApp::update()`) calls
`DutyCycleController::update()` each iteration and checks
`telemetryReady()` to know when to build and enqueue a packet.

## Phases

```
NotStarted
  ↓ begin()
IdleSleeping ←────────────────────────────────────┐
  ↓ sleep elapsed or threshold crossed             │
WarmingUp                                          │
  ↓ warmupMs elapsed                               │
ActiveSampling                                     │
  ↓ activeSampleMs elapsed + markTelemetrySent()   │
CooldownSleeping ─────────────────────────────────┘
  (after cooldown → IdleSleeping)

Error (fatal sensor failure, if failOnSampleError = true)
```

This is the full state machine as implemented, but **it is not what runs
today** — every current build has `cfg.enabled = false`
(`SensingConfig::DutyCycle::kThresholdEnabled`), and `DutyCycleController::update()`
short-circuits to a reduced path while disabled:

- `begin()` always transitions `NotStarted → WarmingUp` unconditionally.
- While disabled, `IdleSleeping`'s threshold-trigger logic (`updateSleeping()`)
  is never invoked — the controller instead treats `IdleSleeping`/`NotStarted`
  the same as `WarmingUp`, running `updateWakingSensors()` and waiting out the
  real `warmupMs`.
- Once `ActiveSampling` is reached, the controller **stays there permanently**,
  sampling every `samplePeriodMs` — `activeSampleMs` is never checked, so it
  never transitions to `CooldownSleeping` on its own.
- If `CooldownSleeping` is ever entered (not reachable in practice while
  disabled), it jumps straight back to `ActiveSampling`, skipping the real
  `minSleepMs` wait.

Net effect while disabled: one `WarmingUp` delay at boot, then indefinite
`ActiveSampling` at `samplePeriodMs` — `IdleSleeping` and `CooldownSleeping`
are never functionally exercised.

## Configuration

`DutyCycleConfig` is built by the single factory `DutyCycleConfig::make(...)`
(`include/power/DutyCycleController.h`) — there is no separate
`dutyCycleCfgContinuous()` / `dutyCycleCfg()` pair of factories. Two named
constant sets exist in `SensingConfig::DutyCycle` (`kThreshold*` and
`kContinuous*`); `main.cpp` currently wires up the **`kThreshold*`** set.

> **Authoritative values:** `platformio/include/config/SensingConfig.h` — `SensingConfig::DutyCycle` namespace.
> For the full parameter table see [TUNABLE_PARAMETERS.md](TUNABLE_PARAMETERS.md#sensing--duty-cycle).

### Current shipped behavior

`kThresholdEnabled` is `false`, so every current build runs with the duty-cycle
gate **disabled** regardless of which named constant set is wired in — the
controller never cycles through `IdleSleeping` → `WarmingUp` →
`ActiveSampling` → `CooldownSleeping`. Sensors are instead serviced
back-to-back at `samplePeriodMs = 750 ms` (`kThresholdSamplePeriodMs`).

| Parameter | Value (as wired by `main.cpp`) | Meaning |
|---|---|---|
| `minSleepMs` | 3 000 ms | Minimum time in `IdleSleeping` before waking (unused while disabled) |
| `maxWakeMs` | 1 000 ms | Max additional wake delay (unused while disabled) |
| `warmupMs` | 10 000 ms | Time in `WarmingUp` — sensor stabilization delay |
| `activeSampleMs` | 30 000 ms | Duration of the `ActiveSampling` window (unused while disabled) |
| `samplePeriodMs` | 750 ms | Master loop cadence while disabled — how often sensors are serviced |
| `tempDeltaThresholdC` | 1.0 °C | Threshold to trigger early wake from idle (unused while disabled) |
| `humidityDeltaThresholdPct` | 5.0 %RH | Threshold to trigger early wake from idle (unused while disabled) |
| `failOnSampleError` | false | Whether sensor errors are fatal |

### Continuous constant set (defined, not currently wired up)

`SensingConfig::DutyCycle::kContinuous*` is a second named constant set with
the same `enabled = false` behavior as Threshold, differing in its temp/humidity
delta thresholds (both `0.0`, vs. Threshold's `1.0`/`5.0`) and `samplePeriodMs`
(`750 ms`, same value as Threshold today). No current build references it —
`main.cpp` only constructs `DutyCycleConfig::make(...)` from the `kThreshold*`
constants.

## Trigger Sensor

`DutyCycleController` takes an `ITriggerSensor` reference (the SHT31
temperature/humidity sensor, per `main.cpp`). When in `IdleSleeping`, the
trigger sensor is polled each update tick. If the reading crosses
`tempDeltaThresholdC` or `humidityDeltaThresholdPct` relative to the baseline
established at the end of the last `ActiveSampling` window, the controller
wakes early.

This allows the node to respond to rapid environmental change without burning
power during quiet conditions — **when duty cycling is enabled**. With
`cfg.enabled = false` (current shipped behavior), `IdleSleeping`'s trigger
logic is never invoked, so this mechanism is currently dormant.

## Sample Dispatch

During `ActiveSampling`, `serviceAllSensors()` calls
`ISensor::update()` on every registered sensor each loop iteration. Sensors
write their readings into a shared `SensorSnapshot` via `fillSnapshot()` when
the application calls them.

`markTelemetrySent()` must be called by the application after each snapshot is
consumed. This resets the `telemetryReady` flag so the controller does not
repeatedly signal readiness before transitioning to `CooldownSleeping`.

## Sensor Sleep / Wake

Duty-cycled sensors implement `ITriggerSensor::wake()` and
`ITriggerSensor::sleep()`. Calling `sleepDutyCycledSensors()` /
`wakeDutyCycledSensors()` toggles power or standby states where the hardware
supports it. Sensors that do not support sleep are left on during the idle
period.

## Error Handling

If `failOnSampleError = false` (the default), a sensor failure during sampling
transitions the controller to `CooldownSleeping` without emitting a snapshot
rather than halting the node. This keeps the node running and transmitting what
it can while a single sensor is faulty.

Setting `failOnSampleError = true` causes the controller to enter the `Error`
phase permanently on the first sensor failure, useful during initial bring-up
to surface problems quickly.

## Relationship to TDMA

`DutyCycleController` has no knowledge of TDMA timing. It fires
`telemetryReady()` based purely on elapsed time and sensor data. The application
is responsible for calling `enqueueTelemetry()` on `TdmaRadioService`, which
then gates the actual over-the-air transmission to the correct TDMA slot.

The sensing pipeline and the radio pipeline are fully independent — sensing is
not paused while the radio transmits.
