#include "power/DutyCycleController.h"

DutyCycleController::DutyCycleController(const DutyCycleConfig &cfg,
                                         ISensor **sensors,
                                         size_t sensorCount,
                                         IClock &clock)
    : _cfg(cfg),
      _sensors(sensors),
      _sensorCount(sensorCount),
      _clock(clock) {}

bool DutyCycleController::begin() {
  _error = DutyCycleError::None;

  if (!beginSensors()) {
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  if (!sleepDutyCycledSensors()) {
    transitionTo(DutyCyclePhase::Error);
    return false;
  }

  transitionTo(DutyCyclePhase::Sleeping);
  return true;
}

void DutyCycleController::update() {
  switch (_phase) {
  case DutyCyclePhase::Sleeping:
    updateSleeping();
    break;

  case DutyCyclePhase::WakingSensors:
    updateWakingSensors();
    break;

  case DutyCyclePhase::Sampling:
    updateSampling();
    break;

  case DutyCyclePhase::TelemetryReady:
  case DutyCyclePhase::Error:
  case DutyCyclePhase::NotStarted:
  default:
    break;
  }
}

void DutyCycleController::markTelemetrySent() {
  if (_phase != DutyCyclePhase::TelemetryReady) {
    return;
  }

  sleepDutyCycledSensors();
  transitionTo(DutyCyclePhase::Sleeping);
}

DutyCyclePhase DutyCycleController::phase() const {
  return _phase;
}

DutyCycleError DutyCycleController::error() const {
  return _error;
}

bool DutyCycleController::telemetryReady() const {
  return _phase == DutyCyclePhase::TelemetryReady;
}

uint32_t DutyCycleController::phaseElapsedMs() const {
  return _clock.millis() - _phaseStartMs;
}

void DutyCycleController::transitionTo(DutyCyclePhase next) {
  _phase = next;
  _phaseStartMs = _clock.millis();
}

void DutyCycleController::updateSleeping() {
  if (phaseElapsedMs() < _cfg.sleepMs) {
    return;
  }

  if (!wakeDutyCycledSensors()) {
    transitionTo(DutyCyclePhase::Error);
    return;
  }

  transitionTo(DutyCyclePhase::WakingSensors);
}

void DutyCycleController::updateWakingSensors() {
  bool allReady = true;

  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    if (sensor->dutyClass() == SensorDutyClass::AlwaysOn) {
      continue;
    }

    sensor->service();

    if (!sensor->ready()) {
      allReady = false;
    }
  }

  if (allReady) {
    transitionTo(DutyCyclePhase::Sampling);
    return;
  }

  if (phaseElapsedMs() >= _cfg.maxWakeMs) {
    _error = DutyCycleError::SensorWakeTimeout;
    transitionTo(DutyCyclePhase::Error);
  }
}

void DutyCycleController::updateSampling() {
  bool allSamplesOk = true;

  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    if (!sensor->ready()) {
      allSamplesOk = false;
      continue;
    }

    if (!sensor->sample()) {
      allSamplesOk = false;
    }
  }

  if (!allSamplesOk && _cfg.failOnSampleError) {
    _error = DutyCycleError::SensorSampleFailed;
    transitionTo(DutyCyclePhase::Error);
    return;
  }

  transitionTo(DutyCyclePhase::TelemetryReady);
}

bool DutyCycleController::beginSensors() {
  for (size_t i = 0; i < _sensorCount; ++i) {
    ISensor *sensor = _sensors[i];

    if (!sensor) {
      continue;
    }

    if (!sensor->begin()) {
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
