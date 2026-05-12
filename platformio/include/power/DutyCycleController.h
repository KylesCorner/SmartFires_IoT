#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include "sensors/ITriggerSensor.h"

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
  uint32_t warmupMs;

  float tempDeltaThresholdC;
  float humidityDeltaThresholdPct;
  bool failOnSampleError;
  bool enabled;

  static DutyCycleConfig dutyCycleCfg(
                                      bool enabled_ = true,uint32_t minSleepMs_ = 3000,
                                      uint32_t maxWakeMs_ = 1000,
                                      uint32_t activeSampleMs_ = 30000,
                                      uint32_t samplePeriodMs_ = 500,
                                      uint32_t warmupMs_ = 10000,
                                      float tempDeltaThresholdC_ = 1.0f,
                                      float humidityDeltaThresholdPct_ = 5.0f,
                                      bool failOnSampleError_ = false) {
    DutyCycleConfig cfg;
    cfg.minSleepMs = minSleepMs_;
    cfg.maxWakeMs = maxWakeMs_;
    cfg.failOnSampleError = failOnSampleError_;
    cfg.activeSampleMs = activeSampleMs_;
    cfg.samplePeriodMs = samplePeriodMs_;
    cfg.warmupMs = warmupMs_;
    cfg.tempDeltaThresholdC = tempDeltaThresholdC_;
    cfg.humidityDeltaThresholdPct = humidityDeltaThresholdPct_;
    cfg.failOnSampleError = failOnSampleError_;
    cfg.enabled = enabled_;
    return cfg;
  }
};

class DutyCycleController {
public:
  DutyCycleController(const DutyCycleConfig &cfg, ITriggerSensor &triggerSensor,
                      ISensor **sensors, size_t sensorCount, IClock &clock);

  bool begin();
  void update();

  void markTelemetrySent();

  DutyCyclePhase phase() const;
  DutyCycleError error() const;

  bool telemetryReady() const;
  uint32_t phaseElapsedMs() const;
  void changeEnableState(bool enabled);

private:
  DutyCycleConfig _cfg;
  ISensor **_sensors;
  size_t _sensorCount;
  IClock &_clock;
  bool _freshSampleReady = false;

  DutyCyclePhase _phase = DutyCyclePhase::NotStarted;
  DutyCycleError _error = DutyCycleError::None;

  ITriggerSensor &_triggerSensor;

  float _baselineTempC = NAN;
  float _baselineHumidityPct = NAN;

  uint32_t _phaseStartMs = 0;
  uint32_t _lastSampleMs = 0;

  void transitionTo(DutyCyclePhase next);

  bool thresholdCrossed(const ITriggerSensor::Reading &r) const;
  void updateSleeping();
  void updateWakingSensors();
  void updateSampling();
  void updateCooldownSleeping();

  bool beginSensors();
  bool sleepDutyCycledSensors();
  bool wakeDutyCycledSensors();
  void serviceAllSensors();


};
