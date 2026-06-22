#include "power/DutyCycleController.h"

#include "logging/DebugLogger.h"
#include "power/BatteryMonitor.h"
#include "sensors/ITriggerSensor.h"

#include <Arduino.h>
#include <math.h>

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

void DutyCycleController::transitionTo(DutyCyclePhase next) {
  const DutyCyclePhase prev = _phase;
  const uint32_t now = _clock.millis();
  const uint32_t elapsed = now - _phaseStartMs;

  _phase = next;
  _phaseStartMs = now;

  LOG_INFO("duty", "transition from=%s to=%s elapsed_ms=%lu", phaseName(prev),
           phaseName(next), static_cast<unsigned long>(elapsed));

  if (next == DutyCyclePhase::ActiveSampling) {
    _freshSampleReady = false;
    _lastSampleMs = _clock.millis() - _cfg.samplePeriodMs;

    LOG_DEBUG("duty", "active_sampling_enter force_first_sample=1");
  }
}

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

void DutyCycleController::updateSleeping() {
  if (_triggerSensor.ready()) {
    if (!_triggerSensor.sample()) {
      LOG_WARN("trigger", "sample_failed phase=%s", phaseName(_phase));
    }

    const auto &r = _triggerSensor.triggerReading();

    if (r.valid && isnan(_baselineTempC)) {
      _baselineTempC = r.tempC;
      _baselineHumidityPct = r.humidityPct;

      LOG_INFO("trigger", "baseline_set temp_c=%.2f humidity_pct=%.2f",
               _baselineTempC, _baselineHumidityPct);
    }

    if (r.valid && phaseElapsedMs() >= _cfg.minSleepMs && thresholdCrossed(r)) {
      if (!wakeDutyCycledSensors()) {
        LOG_ERROR("duty", "wake_after_trigger_failed error=%d",
                  static_cast<int>(_error));
        transitionTo(DutyCyclePhase::Error);
        return;
      }

      LOG_INFO("trigger", "triggered waking_sensors=1");
      transitionTo(DutyCyclePhase::WarmingUp);
    }
  }
}

void DutyCycleController::updateCooldownSleeping() {
  if (_triggerSensor.ready()) {
    if (!_triggerSensor.sample()) {
      LOG_WARN("trigger", "sample_failed phase=%s", phaseName(_phase));
    }
  }

  if (phaseElapsedMs() >= _cfg.minSleepMs) {
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

void DutyCycleController::updateSampling() {
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
        if (sensor->sample()) {
          sampledAny = true;

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
      LOG_DEBUG("duty", "fresh_sample_ready=1");
    } else {
      LOG_WARN("duty", "sample_tick_no_sensor_sampled");
    }
  }

  if (!_cfg.enabled) {
    return;
  }

  if (phaseElapsedMs() >= _cfg.activeSampleMs) {
    LOG_INFO("duty",
             "active_window_complete elapsed_ms=%lu active_sample_ms=%lu",
             static_cast<unsigned long>(phaseElapsedMs()),
             static_cast<unsigned long>(_cfg.activeSampleMs));

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
  }
}

bool DutyCycleController::beginSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      LOG_WARN("duty", "begin_skip_null_sensor index=%u",
               static_cast<unsigned int>(i));
      continue;
    }

    const char *sensorName = sensor->name();

    LOG_INFO(sensorName, "begin_start duty_class=%s",
             dutyClassName(sensor->dutyClass()));

    if (!sensor->begin()) {
      LOG_ERROR(sensorName, "begin_failed");
      if (!sensor->reset()) {
        LOG_ERROR(sensorName, "reset_failed");
      } else {
        LOG_INFO(sensorName, "begin_ok_after_reset");
        return true;
      }

      _error = DutyCycleError::SensorBeginFailed;
      return false;
    }

    LOG_INFO(sensorName, "begin_ok");
  }

  return true;
}

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
