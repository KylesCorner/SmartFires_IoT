#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include <stddef.h>
#include <stdint.h>

enum class DutyCyclePhase : uint8_t {
  NotStarted,
  Sleeping,
  WakingSensors,
  Sampling,
  TelemetryReady,
  Error
};

enum class DutyCycleError : uint8_t {
  None,
  SensorBeginFailed,
  SensorWakeFailed,
  SensorWakeTimeout,
  SensorSampleFailed
};

struct DutyCycleConfig {
  uint32_t sleepMs = 30000;
  uint32_t maxWakeMs = 10000;
  bool failOnSampleError = false;
};

class DutyCycleController {
public:
  DutyCycleController(const DutyCycleConfig &cfg,
                      ISensor **sensors,
                      size_t sensorCount,
                      IClock &clock);

  bool begin();
  void update();

  void markTelemetrySent();

  DutyCyclePhase phase() const;
  DutyCycleError error() const;

  bool telemetryReady() const;
  uint32_t phaseElapsedMs() const;

private:
  DutyCycleConfig _cfg;
  ISensor **_sensors;
  size_t _sensorCount;
  IClock &_clock;

  DutyCyclePhase _phase = DutyCyclePhase::NotStarted;
  DutyCycleError _error = DutyCycleError::None;

  uint32_t _phaseStartMs = 0;

  void transitionTo(DutyCyclePhase next);

  void updateSleeping();
  void updateWakingSensors();
  void updateSampling();

  bool beginSensors();
  bool sleepDutyCycledSensors();
  bool wakeDutyCycledSensors();
};
