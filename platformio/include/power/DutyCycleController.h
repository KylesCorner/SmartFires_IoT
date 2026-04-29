#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include "sensors/Sht31Sensor.h"

#include <stddef.h>
#include <stdint.h>

enum class DutyCyclePhase : uint8_t {
  NotStarted,
  IdleSleeping,
  WarmingUp,
  ActiveSampling,
  CooldownSleeping,
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
  uint32_t minSleepMs;
  uint32_t maxWakeMs;
  uint32_t activeSampleMs;
  uint32_t samplePeriodMs;

  float tempDeltaThresholdC;
  float humidityDeltaThresholdPct;
  bool failOnSampleError;

  static DutyCycleConfig dutyCycleCfg(uint32_t minSleepMs_ = 3000,
                                      uint32_t maxWakeMs_ = 100000,
                                      uint32_t activeSampleMs_ = 300000,
                                      uint32_t samplePeriodMs_ = 1000,
                                      float tempDeltaThresholdC_ = 1.0f,
                                      float humidityDeltaThresholdPct_ = 5.0f,
                                      bool failOnSampleError_ = false) {
    DutyCycleConfig cfg;
    cfg.minSleepMs = minSleepMs_;
    cfg.maxWakeMs = maxWakeMs_;
    cfg.failOnSampleError = failOnSampleError_;
    cfg.activeSampleMs = activeSampleMs_;
    cfg.samplePeriodMs = samplePeriodMs_;
    cfg.tempDeltaThresholdC = tempDeltaThresholdC_;
    cfg.humidityDeltaThresholdPct = humidityDeltaThresholdPct_;
    cfg.failOnSampleError = failOnSampleError_;
    return cfg;
  }
};

class DutyCycleController {
public:
  DutyCycleController(const DutyCycleConfig &cfg, Sht31Sensor &triggerSensor,
                      ISensor **sensors, size_t sensorCount, IClock &clock);

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

  Sht31Sensor &_triggerSensor;

  float _baselineTempC = NAN;
  float _baselineHumidityPct = NAN;

  uint32_t _phaseStartMs = 0;
  uint32_t _lastSampleMs = 0;

  void transitionTo(DutyCyclePhase next);

  bool thresholdCrossed(const Sht31Sensor::Reading &r) const;
  void updateSleeping();
  void updateWakingSensors();
  void updateSampling();
  void updateCooldownSleeping();

  bool beginSensors();
  bool sleepDutyCycledSensors();
  bool wakeDutyCycledSensors();


};
