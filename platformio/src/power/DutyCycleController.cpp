#include "power/DutyCycleController.h"
#include "sensors/ITriggerSensor.h"
#include <Arduino.h>

DutyCycleController::DutyCycleController(const DutyCycleConfig &cfg,
                                         ITriggerSensor &triggerSensor,
                                         ISensor **sensors, size_t sensorCount,
                                         IClock &clock)
    : _cfg(cfg), _sensors(sensors), _sensorCount(sensorCount), _clock(clock),
      _triggerSensor(triggerSensor) {}

bool DutyCycleController::begin() {
  _error = DutyCycleError::None;

  if (!beginSensors()) {
    transitionTo(DutyCyclePhase::Error);
    // Serial.println("Duty failed to begin sensors");
    return false;
  }

  if (!sleepDutyCycledSensors()) {
    // Serial.println("Duty failed to begin sleep duty cycled sensors");
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  if (!wakeDutyCycledSensors()) {
    // Serial.println("Duty failed to wake duty cycled sensors");
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  transitionTo(DutyCyclePhase::WarmingUp);
  return true;
}

void DutyCycleController::update() {
  switch (_phase) {
  case DutyCyclePhase::IdleSleeping:
    // Serial.println("Duty cycle: sleep");
    updateSleeping();
    break;

  case DutyCyclePhase::WarmingUp:
    // Serial.println("Duty cycle: waking");
    updateWakingSensors();
    break;

  case DutyCyclePhase::ActiveSampling:
    // Serial.println("Duty cycle: sampling");
    updateSampling();
    break;

  case DutyCyclePhase::CooldownSleeping:
    // Serial.println("Duty cycle: cooling down");
    updateCooldownSleeping();
    break;

  case DutyCyclePhase::Error:
    // Serial.println("Duty cycle: error");
    break;
  case DutyCyclePhase::NotStarted:
    // Serial.println("Duty cycle: NotStarted");
    break;
  default:
    break;
  }
}

void DutyCycleController::markTelemetrySent() {
  if (_phase != DutyCyclePhase::ActiveSampling || !_freshSampleReady) {
    return;
  }

  _freshSampleReady = false;
  // sleepDutyCycledSensors();
  // transitionTo(DutyCyclePhase::CooldownSleeping);
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
  _phase = next;
  _phaseStartMs = _clock.millis();

  if (next == DutyCyclePhase::ActiveSampling) {
    _freshSampleReady = false;
    // _lastSampleMs = 0; // force first sample immediately
    _lastSampleMs = _clock.millis() - _cfg.samplePeriodMs;
  }
}
bool DutyCycleController::thresholdCrossed(
    const ITriggerSensor::Reading &r) const {
  if (isnan(_baselineTempC) || isnan(_baselineHumidityPct)) {
    return false;
  }

  const float tempDelta = fabsf(r.tempC - _baselineTempC);
  const float humidityDelta = fabsf(r.humidityPct - _baselineHumidityPct);

  return tempDelta >= _cfg.tempDeltaThresholdC ||
         humidityDelta >= _cfg.humidityDeltaThresholdPct;
}

void DutyCycleController::updateSleeping() {
  _triggerSensor.service();

  if (_triggerSensor.ready()) {
    _triggerSensor.sample();

    const auto &r = _triggerSensor.triggerReading();

    if (r.valid && isnan(_baselineTempC)) {
      _baselineTempC = r.tempC;
      _baselineHumidityPct = r.humidityPct;
    }

    if (r.valid && phaseElapsedMs() >= _cfg.minSleepMs && thresholdCrossed(r)) {
      wakeDutyCycledSensors();
      transitionTo(DutyCyclePhase::WarmingUp);
    }
  }
}
void DutyCycleController::updateCooldownSleeping() {
  _triggerSensor.service();

  if (_triggerSensor.ready()) {
    _triggerSensor.sample();
  }

  if (phaseElapsedMs() >= _cfg.minSleepMs) {
    transitionTo(DutyCyclePhase::IdleSleeping);
  }
}

void DutyCycleController::updateWakingSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    sensor->service();
  }

  if (phaseElapsedMs() >= _cfg.warmupMs) {
    transitionTo(DutyCyclePhase::ActiveSampling);
  }
}

void DutyCycleController::updateSampling() {
  if (_clock.millis() - _lastSampleMs >= _cfg.samplePeriodMs) {
    _lastSampleMs = _clock.millis();

    bool sampledAny = false;

    for (size_t i = 0; i < _sensorCount; ++i) {
      ISensor *sensor = _sensors[i];

      if (!sensor) {
        continue;
      }

      sensor->service();
      if (sensor->ready()) {
        if (sensor->sample()) {
          sampledAny = true;
          Serial.print(sensor->name());
          Serial.println(" Sampled");
          // char buf[180];
          // sensor->writeTelemetry(buf, sizeof(buf));
          // Serial.println(buf);

        } else {
          Serial.print(sensor->name());
          Serial.println(" Sampled Failed!");
        }
      }

      // if (sensor->ready()) {
      //   // Serial.print(sensor->name());
      //   // Serial.println(" Sampled");
      //   // sensor->sample();
      //   sampledAny = true;
      // } else {
      //   // Serial.print(sensor->name());
      //   // Serial.println(" Sample failed, not ready");
      // }
    }

    if (sampledAny) {
      _freshSampleReady = true;
    }
  }

  if (phaseElapsedMs() >= _cfg.activeSampleMs) {
    sleepDutyCycledSensors();

    const auto &r = _triggerSensor.triggerReading();
    if (r.valid) {
      _baselineTempC = r.tempC;
      _baselineHumidityPct = r.humidityPct;
    }

    _freshSampleReady = false;
    transitionTo(DutyCyclePhase::CooldownSleeping);
  }
}

bool DutyCycleController::beginSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    if (!sensor->begin()) {
      // Serial.print(sensor->name());
      // Serial.println(" Failed to begin.");

      _error = DutyCycleError::SensorBeginFailed;
      return false;
    }
  }

  return true;
}

bool DutyCycleController::sleepDutyCycledSensors() {
  bool ok = true;

  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    if (sensor->dutyClass() == SensorDutyClass::AlwaysOn) {
      continue;
    }

    if (!sensor->sleep()) {
      ok = false;
    }
  }

  return ok;
}

bool DutyCycleController::wakeDutyCycledSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    if (sensor->dutyClass() == SensorDutyClass::AlwaysOn) {
      continue;
    }

    if (!sensor->wake()) {
      _error = DutyCycleError::SensorWakeFailed;
      return false;
    }
  }

  return true;
}
