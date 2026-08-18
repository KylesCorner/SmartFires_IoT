---
name: duty-cycling
description: DutyCycleController's wake/sample/sleep state machine and its config/trigger sensor.
category: architecture
status: current
last_verified: 2026-08-17
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

This is the full state machine as implemented. Whether a build actually
exercises all of it depends on a compile-time flag — see below.

When `cfg.enabled = false`, `DutyCycleController::update()` short-circuits to
a reduced path:

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
(`include/power/DutyCycleController.h`). `SensingConfig.h` resolves one complete
profile into a `kActive*` constant set, and `main.cpp` constructs the config from
`kActive*` exclusively — it never references a named profile directly.

> **Authoritative values:** `platformio/include/config/SensingConfig.h` — `SensingConfig::DutyCycle` namespace.
> For the full parameter table see [TUNABLE_PARAMETERS.md](TUNABLE_PARAMETERS.md#sensing--duty-cycle).

### Profile selection: `SMARTFIRES_DUTY_CYCLE_MODE`

| Value | `DutyCycleMode` | Constant set | Used by |
|---|---|---|---|
| `0` | `Continuous` | `kContinuous*` | — |
| `1` (default if unset) | `SensorTriggered` | `kSensorTriggered*` | `feather_m0_lora_node` |
| `2` | `Timed` | `kTimed*` | `feather_m0_lora_node_debug`, `feather_m0_lora_node_timed` |
| `3` | `Hybrid` | `kHybrid*` | `feather_m0_lora_node_hybrid` — **deprecated, not a target for further work** |

An unrecognised value is a compile error, not a silent fallback.

Only `Timed` performs real MCU standby: `SmartFiresNodeApp::maybeEnterTimedMcuSleep()`
returns early unless `DutyCycleController::mode() == DutyCycleMode::Timed`. The other
modes idle in the sleeping phases with the CPU running.

### `kSensorTriggered*` — real node build

Full state machine, waking early on a temp/humidity threshold crossing. There is no
scheduled timer wakeup (`kSensorTriggeredTimedSleepMs = 0`).

| Parameter | Value | Meaning |
|---|---|---|
| `minSleepMs` | 3 000 ms | Minimum time in `IdleSleeping` before a trigger may wake |
| `maxWakeMs` | 1 000 ms | Max additional wake delay |
| `warmupMs` | 10 000 ms | Time in `WarmingUp` — sensor stabilization delay |
| `activeSampleMs` | 30 000 ms | Duration of the `ActiveSampling` window |
| `samplePeriodMs` | 750 ms | Sample cadence within `ActiveSampling` |
| `tempDeltaThresholdC` | 1.0 °C | Threshold to trigger early wake from idle |
| `humidityDeltaThresholdPct` | 5.0 %RH | Threshold to trigger early wake from idle |
| `failOnSampleError` | false | Whether sensor errors are fatal |

### `kTimed*` — scheduled wake, MCU standby

Sleeps a fixed interval between active windows; trigger thresholds are ignored.

| Parameter | Value | Meaning |
|---|---|---|
| `timedSleepMs` | 35 000 ms | Sleep between active windows — the MCU standby duration |
| `activeSampleMs` | 25 000 ms | Duration of the `ActiveSampling` window |
| `samplePeriodMs` | 1 000 ms | Sample cadence within `ActiveSampling` |
| `warmupMs` | 10 000 ms | Time in `WarmingUp` after each wake |
| `minSleepMs` | 0 ms | `CooldownSleeping` hands straight over to `IdleSleeping` |
| `failOnSampleError` | false | Whether sensor errors are fatal |

### `kContinuous*` — duty-cycle gate disabled

`enabled = false`, so sensors run back-to-back at `samplePeriodMs` (750 ms), skipping
`IdleSleeping`/`CooldownSleeping` entirely per the disabled-path behavior above. One
`warmupMs` (10 000 ms) delay at boot, then indefinite `ActiveSampling`. Sleep durations
and thresholds are unused.

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

## Active Windows on the Wire (Timed mode)

The controller itself has no notion of a "window" — but in `Timed` mode each
`ActiveSampling` stretch is one, and `SmartFiresNodeApp::updateWindowMarkers()`
watches the phase edges to mark it on the wire via `PktHeader::flags`:

| Edge | Call | Effect |
|---|---|---|
| → `ActiveSampling` | `PacketHandler::beginWindow()` | The window's first bundle is stamped `PKT_FLAG_WINDOW_FIRST` |
| `ActiveSampling` → sleeping | `PacketHandler::flushWindow()` | The partial bundle still in the accumulator is force-encoded and stamped `PKT_FLAG_WINDOW_LAST` |

The flush matters beyond labelling. A bundle is normally only encoded once
`maxDeltas + 1` samples have accumulated, so with `kTimedActiveSampleMs = 25000`
and `kTimedSamplePeriodMs = 1000` a window produces 25 samples: one full
15-sample bundle, and 10 samples left in the accumulator. Without the flush
those samples sit there across the entire standby and are only transmitted
partway into the *next* window, timestamped from the previous one. The flush
sends them while they are still current.

`SmartFiresNodeApp::maybeEnterTimedMcuSleep()` therefore also holds off standby
while `TdmaRadioService::queuedCount() > 0`, up to
`SensingConfig::DutyCycle::kMaxTxDrainBeforeStandbyMs` (5 s ≈ one TDMA frame
plus slack) — otherwise the just-flushed bundle would be parked in the queue for
the whole sleep, exactly the problem the flush was added to solve. Time spent
draining comes out of the sleep, not the next active window, since
`timedSleepRemainingMs()` is re-read after the wait.

Draining also requires the radio to stay powered: the phase is already a
sleeping one at that point, and `TdmaRadioService::update()` returns *before*
`drainTxQueue()` whenever the radio is duty-slept, so leaving phase-based radio
sleep to apply would make the queue impossible to empty and guarantee the drain
burned its full budget every cycle. `SmartFiresNodeApp::radioMustStayAwakeToDrain()`
suppresses radio duty-sleep for exactly that interval.

### What survives the standby

SAMD21 standby retains SRAM — it is not a reset — so `TdmaTxQueue`,
`PacketHandler`'s accumulator, and `TdmaRadioService`'s pending window all come
back byte-identical. Nothing needs to be persisted or pre-flushed for memory
reasons; the drain gate above exists only so the window-close bundle isn't
*delayed* by a whole sleep, not because it would be lost.

What does not survive on its own is the acknowledgement loop, since the radio is
off for the entire sleep. That is handled in two places, both documented in
[PACKET_RELIABILITY.md](PACKET_RELIABILITY.md#duty-cycled-nodes-timed-mode):

- `TdmaRadioService::notifyMcuStandby()` excludes the sleep from the pending
  window's age, so unacked bundles survive to be retransmitted (stamped
  `PKT_FLAG_RETX`) during the next `WarmingUp` phase, when the node's slot is
  otherwise idle.
- The base defers `ACK_SUMMARY` for a node that just sent a fresh `WINDOW_LAST`
  rather than blocking on `sendToWait()` against a radio that is switched off.

## Relationship to TDMA

`DutyCycleController` has no knowledge of TDMA timing. It fires
`telemetryReady()` based purely on elapsed time and sensor data. The application
is responsible for calling `enqueueTelemetry()` on `TdmaRadioService`, which
then gates the actual over-the-air transmission to the correct TDMA slot.

The sensing pipeline and the radio pipeline are fully independent — sensing is
not paused while the radio transmits.
