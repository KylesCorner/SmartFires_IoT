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

## Configuration

Two preset configurations are defined in `DutyCycleConfig`:

### Duty-cycle mode (production)

Used when `enabled = true`. Sensors are powered down between active windows to
reduce current draw.

> **Authoritative values:** `platformio/include/config/SensingConfig.h` — `SensingConfig::DutyCycle` namespace.
> For the full parameter table see [TUNABLE_PARAMETERS.md](TUNABLE_PARAMETERS.md#sensing--duty-cycle).

| Parameter | Default value | Meaning |
|---|---|---|
| `minSleepMs` | 3 000 ms | Minimum time in `IdleSleeping` before waking |
| `maxWakeMs` | 1 000 ms | Max additional wake delay (unused in current impl) |
| `warmupMs` | 10 000 ms | Time in `WarmingUp` — sensor stabilization delay |
| `activeSampleMs` | 30 000 ms | Duration of the `ActiveSampling` window |
| `samplePeriodMs` | 50 000 ms | Target cycle period (wake-to-wake) |
| `tempDeltaThresholdC` | 1.0 °C | Threshold to trigger early wake from idle |
| `humidityDeltaThresholdPct` | 5.0 %RH | Threshold to trigger early wake from idle |
| `failOnSampleError` | false | Whether sensor errors are fatal |

### Continuous mode

Used when `enabled = false`. Sensors run continuously at the fastest practical
rate. `samplePeriodMs = 2 000 ms` gives approximately 0.5 Hz ground truth, but
the bundle system accumulates 15 samples before emitting a packet, so the
effective bundle rate depends on the sample rate fed into `PacketHandler`.

## Trigger Sensor

`DutyCycleController` takes an `ITriggerSensor` reference (typically the SHT31
temperature/humidity sensor). When in `IdleSleeping`, the trigger sensor is
polled each update tick. If the reading crosses `tempDeltaThresholdC` or
`humidityDeltaThresholdPct` relative to the baseline established at the end of
the last `ActiveSampling` window, the controller wakes early.

This allows the node to respond to rapid environmental change without burning
power during quiet conditions.

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
