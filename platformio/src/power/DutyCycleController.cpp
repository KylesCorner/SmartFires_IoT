// ---
// description: Implements DutyCycleController's phase transitions (idle/warmup/active/cooldown), trigger-threshold detection, and per-sensor wake/sleep/sample orchestration.
// role: implementation
// docs: [duty-cycling]
// ---
#include "power/DutyCycleController.h"

#include "logging/DebugLogger.h"
#include "power/BatteryMonitor.h"
#include "sensors/ITriggerSensor.h"

#include <Arduino.h>
#include <math.h>

#if defined(LORA_NODE)
#include "platform/Samd21RamMonitor.h"
#endif

namespace {

const char *phaseName(DutyCyclePhase phase) {
  switch (phase) {
  case DutyCyclePhase::NotStarted:
    return "NotStarted";
  case DutyCyclePhase::IdleSleeping:
    return "IdleSleeping";
  case DutyCyclePhase::WarmingUp:
    return "WarmingUp";
  case DutyCyclePhase::ActiveSampling:
    return "ActiveSampling";
  case DutyCyclePhase::CooldownSleeping:
    return "CooldownSleeping";
  case DutyCyclePhase::Error:
    return "Error";
  default:
    return "Unknown";
  }
}

const char *dutyClassName(SensorDutyClass dutyClass) {
  switch (dutyClass) {
  case SensorDutyClass::AlwaysOn:
    return "AlwaysOn";
  case SensorDutyClass::DutyCycled:
    return "DutyCycled";
  case SensorDutyClass::WarmupHeavy:
    return "WarmupHeavy";
  default:
    return "Unknown";
  }
}

} // namespace

DutyCycleController::DutyCycleController(const DutyCycleConfig &cfg,
                                         ITriggerSensor &triggerSensor,
                                         ISensor **sensors, size_t sensorCount,
                                         IClock &clock, BatteryMonitor &battery)
    : _cfg(cfg), _sensors(sensors), _sensorCount(sensorCount), _clock(clock),
      _triggerSensor(triggerSensor), _battery(battery) {}

bool DutyCycleController::begin() {
  _error = DutyCycleError::None;

  LOG_INFO(
      "duty",
      "begin enabled=%u sensor_count=%u warmup_ms=%lu sample_period_ms=%lu "
      "active_sample_ms=%lu min_sleep_ms=%lu",
      _cfg.enabled ? 1 : 0, static_cast<unsigned int>(_sensorCount),
      static_cast<unsigned long>(_cfg.warmupMs),
      static_cast<unsigned long>(_cfg.samplePeriodMs),
      static_cast<unsigned long>(_cfg.activeSampleMs),
      static_cast<unsigned long>(_cfg.minSleepMs));

  if (!_battery.begin()) {
    _error = DutyCycleError::BatteryBeginFailed;
    LOG_ERROR("battery", "begin_failed");
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  LOG_INFO("battery", "begin_ok");

  if (!beginSensors()) {
    LOG_ERROR("duty", "begin_sensors_failed error=%d",
              static_cast<int>(_error));
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  if (!sleepDutyCycledSensors()) {
    LOG_ERROR("duty", "initial_sleep_duty_cycled_sensors_failed");
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  if (!wakeDutyCycledSensors()) {
    LOG_ERROR("duty", "initial_wake_duty_cycled_sensors_failed error=%d",
              static_cast<int>(_error));
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  transitionTo(DutyCyclePhase::WarmingUp);
  LOG_INFO("duty", "begin_ok");

  return true;
}

void DutyCycleController::update() {
  serviceAllSensors();
  _battery.sample();

  if (!_cfg.enabled) {
    if (_phase == DutyCyclePhase::WarmingUp ||
        _phase == DutyCyclePhase::IdleSleeping ||
        _phase == DutyCyclePhase::NotStarted) {
      updateWakingSensors();
      return;
    }

    if (_phase == DutyCyclePhase::ActiveSampling) {
      updateSampling();
      return;
    }

    if (_phase == DutyCyclePhase::CooldownSleeping) {
      transitionTo(DutyCyclePhase::ActiveSampling);
      return;
    }

    return;
  }

  switch (_phase) {
  case DutyCyclePhase::IdleSleeping:
    updateSleeping();
    break;

  case DutyCyclePhase::WarmingUp:
    updateWakingSensors();
    break;

  case DutyCyclePhase::ActiveSampling:
    updateSampling();
    break;

  case DutyCyclePhase::CooldownSleeping:
    updateCooldownSleeping();
    break;

  case DutyCyclePhase::Error:
    break;

  case DutyCyclePhase::NotStarted:
    break;

  default:
    break;
  }
}

void DutyCycleController::markTelemetrySent() {
  if (_phase != DutyCyclePhase::ActiveSampling || !_freshSampleReady) {
    return;
  }

  LOG_DEBUG("duty", "telemetry_mark_sent");

  _freshSampleReady = false;
}

DutyCyclePhase DutyCycleController::phase() const { return _phase; }

DutyCycleError DutyCycleController::error() const { return _error; }

bool DutyCycleController::telemetryReady() const {
  return _phase == DutyCyclePhase::ActiveSampling && _freshSampleReady;
}

uint32_t DutyCycleController::phaseElapsedMs() const {
  return _clock.millis() - _phaseStartMs;
}
void DutyCycleController::transitionTo(
    DutyCyclePhase next) {
  const DutyCyclePhase prev = _phase;
  const uint32_t now = _clock.millis();
  const uint32_t elapsed = now - _phaseStartMs;

  _phase = next;
  _phaseStartMs = now;

  LOG_INFO(
      "duty",
      "transition from=%s to=%s elapsed_ms=%lu",
      phaseName(prev),
      phaseName(next),
      static_cast<unsigned long>(elapsed));

  // Leaving a sleeping phase starts a new wake-to-wake cycle. WarmingUp is the
  // only way back into the awake half of the cycle, from either sleeping phase
  // or from begin(), so it is the one place the period clock restarts.
  if (next == DutyCyclePhase::WarmingUp) {
    _cycleStartMs = now;
    _activeSampleCount = 0;

    LOG_DEBUG("duty", "cycle_start cycle_period_ms=%lu",
              static_cast<unsigned long>(_cfg.cyclePeriodMs));
  }

  if (next == DutyCyclePhase::ActiveSampling) {
    _freshSampleReady = false;

    // Force an immediate first sample.
    _lastSampleMs =
        now - _cfg.samplePeriodMs;

    LOG_DEBUG(
        "duty",
        "active_sampling_enter force_first_sample=1");
  }

  if (next == DutyCyclePhase::CooldownSleeping) {
    // This timestamp covers both CooldownSleeping and
    // IdleSleeping. The sleep interval starts when the
    // active window ends.
    _sleepStartMs = now;
    _triggerLatched = false;
    _plannedSleepMs = computePlannedSleepMs();

    LOG_DEBUG(
        "duty",
        "sleep_cycle_enter planned_sleep_ms=%lu cycle_period_ms=%lu "
        "cycle_elapsed_ms=%lu min_sleep_ms=%lu",
        static_cast<unsigned long>(_plannedSleepMs),
        static_cast<unsigned long>(_cfg.cyclePeriodMs),
        static_cast<unsigned long>(now - _cycleStartMs),
        static_cast<unsigned long>(
            _cfg.minSleepMs));
  }
}

// Fixed wake-to-wake period: the standby is whatever remains of cyclePeriodMs
// once this cycle's warmup and active window (including any full-bundle
// overrun) have been spent. Measuring against _cycleStartMs rather than
// subtracting nominal warmup/window figures means real jitter is absorbed by
// the sleep, so the cycle length itself stays put.
uint32_t DutyCycleController::computePlannedSleepMs() const {
  if (_cfg.cyclePeriodMs == 0) {
    return 0;
  }

  const uint32_t cycleElapsedMs = _clock.millis() - _cycleStartMs;
  const uint32_t remainingMs = (_cfg.cyclePeriodMs > cycleElapsedMs)
                                   ? (_cfg.cyclePeriodMs - cycleElapsedMs)
                                   : 0;

  if (remainingMs < _cfg.minStandbyMs) {
    LOG_WARN("duty",
             "cycle_overrun cycle_elapsed_ms=%lu cycle_period_ms=%lu "
             "sleep_floored_to_ms=%lu",
             static_cast<unsigned long>(cycleElapsedMs),
             static_cast<unsigned long>(_cfg.cyclePeriodMs),
             static_cast<unsigned long>(_cfg.minStandbyMs));
    return _cfg.minStandbyMs;
  }

  return remainingMs;
}

void DutyCycleController::setActiveWindowHold(bool hold) {
  _activeWindowHold = hold;
}

uint32_t DutyCycleController::plannedSleepMs() const { return _plannedSleepMs; }

uint16_t DutyCycleController::lastWindowSampleCount() const {
  return _lastWindowSampleCount;
}

bool DutyCycleController::lastWindowOverran() const {
  return _lastWindowOverran;
}

// void DutyCycleController::transitionTo(DutyCyclePhase next) {
//   const DutyCyclePhase prev = _phase;
//   const uint32_t now = _clock.millis();
//   const uint32_t elapsed = now - _phaseStartMs;

//   _phase = next;
//   _phaseStartMs = now;

//   LOG_INFO("duty", "transition from=%s to=%s elapsed_ms=%lu", phaseName(prev),
//            phaseName(next), static_cast<unsigned long>(elapsed));

//   if (next == DutyCyclePhase::ActiveSampling) {
//     _freshSampleReady = false;
//     _lastSampleMs = _clock.millis() - _cfg.samplePeriodMs;

//     LOG_DEBUG("duty", "active_sampling_enter force_first_sample=1");
//   }
// }

bool DutyCycleController::thresholdCrossed(
    const ITriggerSensor::Reading &r) const {
  if (isnan(_baselineTempC) || isnan(_baselineHumidityPct)) {
    return false;
  }

  const float tempDelta = fabsf(r.tempC - _baselineTempC);
  const float humidityDelta = fabsf(r.humidityPct - _baselineHumidityPct);

  const bool crossed = tempDelta >= _cfg.tempDeltaThresholdC ||
                       humidityDelta >= _cfg.humidityDeltaThresholdPct;

  if (crossed) {
    LOG_INFO(
        "trigger",
        "threshold_crossed temp_c=%.2f baseline_temp_c=%.2f temp_delta=%.2f "
        "humidity_pct=%.2f baseline_humidity_pct=%.2f humidity_delta=%.2f",
        r.tempC, _baselineTempC, tempDelta, r.humidityPct, _baselineHumidityPct,
        humidityDelta);
  }

  return crossed;
}

// void DutyCycleController::updateSleeping() {
//   if (_triggerSensor.ready()) {
//     if (!_triggerSensor.sample()) {
//       LOG_WARN("trigger", "sample_failed phase=%s", phaseName(_phase));
//     }

//     const auto &r = _triggerSensor.triggerReading();

//     if (r.valid && isnan(_baselineTempC)) {
//       _baselineTempC = r.tempC;
//       _baselineHumidityPct = r.humidityPct;

//       LOG_INFO("trigger", "baseline_set temp_c=%.2f humidity_pct=%.2f",
//                _baselineTempC, _baselineHumidityPct);
//     }

//     if (r.valid && phaseElapsedMs() >= _cfg.minSleepMs && thresholdCrossed(r)) {
//       if (!wakeDutyCycledSensors()) {
//         LOG_ERROR("duty", "wake_after_trigger_failed error=%d",
//                   static_cast<int>(_error));
//         transitionTo(DutyCyclePhase::Error);
//         return;
//       }

//       LOG_INFO("trigger", "triggered waking_sensors=1");
//       transitionTo(DutyCyclePhase::WarmingUp);
//     }
//   }
// }

void DutyCycleController::updateSleeping() {
  sampleSleepTrigger();
  wakeFromSleepIfNeeded();
}

// void DutyCycleController::updateCooldownSleeping() {
//   if (_triggerSensor.ready()) {
//     if (!_triggerSensor.sample()) {
//       LOG_WARN("trigger", "sample_failed phase=%s", phaseName(_phase));
//     }
//   }

//   if (phaseElapsedMs() >= _cfg.minSleepMs) {
//     transitionTo(DutyCyclePhase::IdleSleeping);
//   }
// }

void DutyCycleController::updateCooldownSleeping() {
  sampleSleepTrigger();

  if (wakeFromSleepIfNeeded()) {
    return;
  }

  if (sleepElapsedMs() >= _cfg.minSleepMs) {
    transitionTo(DutyCyclePhase::IdleSleeping);
  }
}

void DutyCycleController::updateWakingSensors() {
  if (phaseElapsedMs() >= _cfg.warmupMs) {
    LOG_INFO("duty", "warmup_complete elapsed_ms=%lu warmup_ms=%lu",
             static_cast<unsigned long>(phaseElapsedMs()),
             static_cast<unsigned long>(_cfg.warmupMs));

    transitionTo(DutyCyclePhase::ActiveSampling);
  }
}

// The active window closes on activeSampleMs, except that a partial bundle in
// the caller's accumulator (setActiveWindowHold) holds it open to the next
// bundle boundary — bounded by activeOverrunMaxMs so a starved sample tick
// cannot hold the window open indefinitely.
bool DutyCycleController::activeWindowShouldClose() const {
  const uint32_t elapsedMs = phaseElapsedMs();

  if (elapsedMs < _cfg.activeSampleMs) {
    return false;
  }

  if (!_activeWindowHold || _cfg.activeOverrunMaxMs == 0) {
    return true;
  }

  return elapsedMs >= _cfg.activeSampleMs + _cfg.activeOverrunMaxMs;
}

void DutyCycleController::updateSampling() {
  // Tested before the sample tick, not after it. The old order took a full
  // reading of every sensor and then threw it away in the same call, because
  // transitionTo(CooldownSleeping) clears _freshSampleReady before the app ever
  // gets to consume it — so a tick landing on the closing boundary (which, with
  // activeSampleMs an exact multiple of samplePeriodMs, is every single window)
  // was a sensor read paid for and discarded.
  if (_cfg.enabled && activeWindowShouldClose()) {
    const uint32_t elapsedMs = phaseElapsedMs();

    _lastWindowSampleCount = _activeSampleCount;
    _lastWindowOverran = _activeWindowHold;

    LOG_INFO("duty",
             "active_window_complete elapsed_ms=%lu active_sample_ms=%lu "
             "samples=%u overrun_ms=%lu held_open=%u",
             static_cast<unsigned long>(elapsedMs),
             static_cast<unsigned long>(_cfg.activeSampleMs),
             static_cast<unsigned int>(_activeSampleCount),
             static_cast<unsigned long>(elapsedMs > _cfg.activeSampleMs
                                            ? elapsedMs - _cfg.activeSampleMs
                                            : 0),
             _activeWindowHold ? 1 : 0);

    if (_lastWindowOverran) {
      // Closed with the hold still asserted: the accumulator never completed
      // its bundle inside the overrun budget, so the caller will have to
      // force-encode a partial one.
      LOG_WARN("duty",
               "active_window_overrun_cap samples=%u overrun_max_ms=%lu",
               static_cast<unsigned int>(_activeSampleCount),
               static_cast<unsigned long>(_cfg.activeOverrunMaxMs));
    }

    if (!sleepDutyCycledSensors()) {
      LOG_WARN("duty", "sleep_duty_cycled_sensors_partial_failure");
    }

    const auto &r = _triggerSensor.triggerReading();
    if (r.valid) {
      _baselineTempC = r.tempC;
      _baselineHumidityPct = r.humidityPct;

      LOG_INFO("trigger", "baseline_updated temp_c=%.2f humidity_pct=%.2f",
               _baselineTempC, _baselineHumidityPct);
    }

    _freshSampleReady = false;
    transitionTo(DutyCyclePhase::CooldownSleeping);
    return;
  }

  if (_clock.millis() - _lastSampleMs >= _cfg.samplePeriodMs) {
    _lastSampleMs = _clock.millis();

    bool sampledAny = false;

    LOG_DEBUG("duty", "sample_tick phase_elapsed_ms=%lu sensor_count=%u",
              static_cast<unsigned long>(phaseElapsedMs()),
              static_cast<unsigned int>(_sensorCount));

    for (size_t i = 0; i < _sensorCount; ++i) {
      ISensor *sensor = _sensors[i];

      if (!sensor) {
        LOG_WARN("duty", "null_sensor index=%u", static_cast<unsigned int>(i));
        continue;
      }

      const char *sensorName = sensor->name();

      LOG_DEBUG(sensorName, "status ready=%u healthy=%u state=%d duty_class=%s",
                sensor->ready() ? 1 : 0, sensor->healthy() ? 1 : 0,
                static_cast<int>(sensor->powerState()),
                dutyClassName(sensor->dutyClass()));

      if (sensor->ready()) {
#if defined(LORA_NODE)
        gRamMonitor.checkpoint(
            "sensor_sample_pre",
            sensorName);
#endif
        if (sensor->sample()) {
          sampledAny = true;

#if defined(LORA_NODE)
          gRamMonitor.checkpoint(
              "sensor_sample_post",
              sensorName);
#endif

          char buf[180];
          sensor->writeTelemetry(buf, sizeof(buf));

          LOG_DEBUG(sensorName, "%s", buf);
        } else {
          LOG_WARN(sensorName, "sample_failed");
        }
      } else {
        LOG_DEBUG(sensorName, "sample_skipped reason=not_ready");
      }
    }

    char buf[180];
    _battery.writeTelemetry(buf, sizeof(buf));
    LOG_INFO("battery", "%s", buf);

    if (sampledAny) {
      _freshSampleReady = true;

      if (_activeSampleCount < 0xFFFFu) {
        _activeSampleCount++;
      }

      LOG_DEBUG("duty", "fresh_sample_ready=1 window_samples=%u",
                static_cast<unsigned int>(_activeSampleCount));
    } else {
      LOG_WARN("duty", "sample_tick_no_sensor_sampled");
    }
  }
}
bool DutyCycleController::beginSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      LOG_WARN(
          "duty",
          "begin_skip_null_sensor index=%u",
          static_cast<unsigned int>(i));

      continue;
    }

    const char *sensorName = sensor->name();

    LOG_INFO(
        sensorName,
        "begin_start duty_class=%s",
        dutyClassName(sensor->dutyClass()));

#if defined(LORA_NODE)
    gRamMonitor.checkpoint(
        "sensor_begin_pre",
        sensorName);
#endif

    const bool beginOk = sensor->begin();

#if defined(LORA_NODE)
    gRamMonitor.checkpoint(
        "sensor_begin_post",
        sensorName);
#endif

    if (beginOk) {
      LOG_INFO(sensorName, "begin_ok");
      continue;
    }

    LOG_ERROR(sensorName, "begin_failed");

#if defined(LORA_NODE)
    gRamMonitor.checkpoint(
        "sensor_reset_pre",
        sensorName);
#endif

    const bool resetOk = sensor->reset();

#if defined(LORA_NODE)
    gRamMonitor.checkpoint(
        "sensor_reset_post",
        sensorName);
#endif

    if (!resetOk) {
      LOG_ERROR(sensorName, "reset_failed");

      _error = DutyCycleError::SensorBeginFailed;

      return false;
    }

    LOG_INFO(sensorName, "begin_ok_after_reset");

    /*
     * IMPORTANT:
     * Continue initializing the remaining sensors.
     *
     * The old code returned true here and accidentally skipped
     * every remaining sensor.
     */
  }

  return true;
}

// bool DutyCycleController::beginSensors() {
//   for (size_t i = 0; i < _sensorCount; ++i) {
//     ISensor *sensor = _sensors[i];

//     if (!sensor) {
//       LOG_WARN("duty", "begin_skip_null_sensor index=%u",
//                static_cast<unsigned int>(i));
//       continue;
//     }

//     const char *sensorName = sensor->name();

//     LOG_INFO(sensorName, "begin_start duty_class=%s",
//              dutyClassName(sensor->dutyClass()));

// #if defined(LORA_NODE)
//     gRamMonitor.checkpoint(
//         "sensor_begin_pre",
//         sensorName);
// #endif

//     if (!sensor->begin()) {
//       LOG_ERROR(sensorName, "begin_failed");
//       if (!sensor->reset()) {
//         LOG_ERROR(sensorName, "reset_failed");
//       } else {
//         LOG_INFO(sensorName, "begin_ok_after_reset");
//         return true;
//       }

//       _error = DutyCycleError::SensorBeginFailed;
//       return false;
//     }

//     LOG_INFO(sensorName, "begin_ok");
//   }

//   return true;
// }

bool DutyCycleController::sleepDutyCycledSensors() {
  bool ok = true;

  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      LOG_WARN("duty", "sleep_skip_null_sensor index=%u",
               static_cast<unsigned int>(i));
      continue;
    }

    const char *sensorName = sensor->name();

    if (sensor->dutyClass() == SensorDutyClass::AlwaysOn) {
      LOG_DEBUG(sensorName, "sleep_skip reason=always_on");
      continue;
    }

    LOG_DEBUG(sensorName, "sleep_start duty_class=%s",
              dutyClassName(sensor->dutyClass()));

    if (!sensor->sleep()) {
      LOG_WARN(sensorName, "sleep_failed");
      ok = false;
    } else {
      LOG_DEBUG(sensorName, "sleep_ok");
    }
  }

  return ok;
}

bool DutyCycleController::wakeDutyCycledSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      LOG_WARN("duty", "wake_skip_null_sensor index=%u",
               static_cast<unsigned int>(i));
      continue;
    }

    const char *sensorName = sensor->name();

    if (sensor->dutyClass() == SensorDutyClass::AlwaysOn) {
      LOG_DEBUG(sensorName, "wake_skip reason=always_on");
      continue;
    }

    LOG_DEBUG(sensorName, "wake_start duty_class=%s",
              dutyClassName(sensor->dutyClass()));

    if (!sensor->wake()) {
      LOG_ERROR(sensorName, "wake_failed");

      _error = DutyCycleError::SensorWakeFailed;
      return false;
    }

    LOG_DEBUG(sensorName, "wake_ok");
  }

  return true;
}

void DutyCycleController::changeEnableState(bool enabled) {
  const bool oldEnabled = _cfg.enabled;
  _cfg.enabled = enabled;

  LOG_INFO("duty", "enabled_changed from=%u to=%u", oldEnabled ? 1 : 0,
           enabled ? 1 : 0);
}

void DutyCycleController::serviceAllSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    sensor->service();
  }
}
bool DutyCycleController::resetSensors() {
  bool ok = true;

  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    LOG_WARN(sensor->name(), "reset_start_from_controller");

    if (!sensor->reset()) {
      LOG_ERROR(sensor->name(), "reset_failed_from_controller");
      ok = false;
      continue;
    }

    LOG_WARN(sensor->name(), "reset_ok_from_controller state=%d healthy=%u",
             static_cast<int>(sensor->powerState()),
             sensor->healthy() ? 1 : 0);
  }

  return ok;
}
bool DutyCycleController::triggerWakeEnabled() const {
  return
      _cfg.wakeMode ==
          DutyCycleMode::SensorTriggered ||
      _cfg.wakeMode ==
          DutyCycleMode::Hybrid;
}

bool DutyCycleController::timedWakeEnabled() const {
  return
      _cfg.wakeMode ==
          DutyCycleMode::Timed ||
      _cfg.wakeMode ==
          DutyCycleMode::Hybrid;
}

uint32_t
DutyCycleController::sleepElapsedMs() const {
  return _clock.millis() - _sleepStartMs;
}

void DutyCycleController::sampleSleepTrigger() {
  if (!triggerWakeEnabled()) {
    return;
  }

  if (!_triggerSensor.ready()) {
    return;
  }

  if (!_triggerSensor.sample()) {
    LOG_WARN(
        "trigger",
        "sample_failed phase=%s",
        phaseName(_phase));
    return;
  }

  const auto &reading =
      _triggerSensor.triggerReading();

  if (!reading.valid) {
    return;
  }

  if (isnan(_baselineTempC) ||
      isnan(_baselineHumidityPct)) {
    _baselineTempC = reading.tempC;
    _baselineHumidityPct =
        reading.humidityPct;

    LOG_INFO(
        "trigger",
        "baseline_set temp_c=%.2f "
        "humidity_pct=%.2f",
        _baselineTempC,
        _baselineHumidityPct);

    return;
  }

  if (thresholdCrossed(reading)) {
    _triggerLatched = true;

    LOG_INFO(
        "trigger",
        "wake_request_latched "
        "sleep_elapsed_ms=%lu",
        static_cast<unsigned long>(
            sleepElapsedMs()));
  }
}

bool DutyCycleController::wakeFromSleepIfNeeded() {
  const uint32_t elapsed = sleepElapsedMs();

  const bool triggerDue =
      triggerWakeEnabled() &&
      _triggerLatched &&
      elapsed >= _cfg.minSleepMs;

  const bool timerDue =
      timedWakeEnabled() &&
      _plannedSleepMs > 0 &&
      elapsed >= _plannedSleepMs;

  if (!triggerDue && !timerDue) {
    return false;
  }

  const char *reason = nullptr;

  if (triggerDue && timerDue) {
    reason = "trigger_and_timer";
  } else if (triggerDue) {
    reason = "trigger";
  } else {
    reason = "timer";
  }

  LOG_INFO(
      "duty",
      "sleep_wakeup reason=%s "
      "sleep_elapsed_ms=%lu",
      reason,
      static_cast<unsigned long>(elapsed));

  if (!wakeDutyCycledSensors()) {
    LOG_ERROR(
        "duty",
        "wake_from_sleep_failed "
        "reason=%s error=%d",
        reason,
        static_cast<int>(_error));

    transitionTo(DutyCyclePhase::Error);
    return true;
  }

  transitionTo(DutyCyclePhase::WarmingUp);
  return true;
}

DutyCycleMode DutyCycleController::mode() const {
  if (!_cfg.enabled) {
    return DutyCycleMode::Continuous;
  }

  return _cfg.wakeMode;
}

bool DutyCycleController::sleeping() const {
  return
      _phase == DutyCyclePhase::CooldownSleeping ||
      _phase == DutyCyclePhase::IdleSleeping;
}

uint32_t
DutyCycleController::timedSleepRemainingMs() const {
  if (mode() != DutyCycleMode::Timed ||
      !sleeping()) {
    return 0;
  }

  const uint32_t elapsedMs = sleepElapsedMs();

  // _plannedSleepMs was fixed at window close by the cycle-period arithmetic, so
  // time the node spends awake after the close (waiting for its TDMA slot to
  // carry the last bundle and PKT_WINDOW_END) comes out of the standby and the
  // wake-to-wake period holds.
  if (elapsedMs >= _plannedSleepMs) {
    return 0;
  }

  return _plannedSleepMs - elapsedMs;
}