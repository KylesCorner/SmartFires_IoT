#pragma once

#include "config/SensingConfig.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include "sensors/ITriggerSensor.h"

#include "power/BatteryMonitor.h"

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
  BatteryBeginFailed,
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

  // Threshold-triggered profile (DutyCyclePhase wake/sleep state machine).
  // Not used by any build today — kept available as a documented alternate
  // operating mode. Defaults come from config/SensingConfig.h's
  // DutyCycle::kThreshold* constants (single source; values unchanged from
  // before this consolidation, including the carried-over samplePeriodMs
  // "//TEMP" marker).
  static DutyCycleConfig dutyCycleCfg(
      bool enabled_ = SensingConfig::DutyCycle::kThresholdEnabled,
      uint32_t minSleepMs_ = SensingConfig::DutyCycle::kThresholdMinSleepMs,
      uint32_t maxWakeMs_ = SensingConfig::DutyCycle::kThresholdMaxWakeMs,
      uint32_t activeSampleMs_ = SensingConfig::DutyCycle::kThresholdActiveSampleMs,
      uint32_t samplePeriodMs_ = SensingConfig::DutyCycle::kThresholdSamplePeriodMs,
      uint32_t warmupMs_ = SensingConfig::DutyCycle::kThresholdWarmupMs,
      float tempDeltaThresholdC_ = SensingConfig::DutyCycle::kThresholdTempDeltaThresholdC,
      float humidityDeltaThresholdPct_ = SensingConfig::DutyCycle::kThresholdHumidityDeltaThresholdPct,
      bool failOnSampleError_ = SensingConfig::DutyCycle::kThresholdFailOnSampleError) {
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

  // Continuous profile — what every production node build ships today
  // (called with no arguments from main.cpp). Defaults come from
  // config/SensingConfig.h's DutyCycle::kContinuous* constants.
  static DutyCycleConfig dutyCycleCfgContinuous(
      bool enabled_ = SensingConfig::DutyCycle::kContinuousEnabled,
      uint32_t minSleepMs_ = SensingConfig::DutyCycle::kContinuousMinSleepMs,
      uint32_t maxWakeMs_ = SensingConfig::DutyCycle::kContinuousMaxWakeMs,
      uint32_t activeSampleMs_ = SensingConfig::DutyCycle::kContinuousActiveSampleMs,
      uint32_t samplePeriodMs_ = SensingConfig::DutyCycle::kContinuousSamplePeriodMs,
      uint32_t warmupMs_ = SensingConfig::DutyCycle::kContinuousWarmupMs,
      float tempDeltaThresholdC_ = SensingConfig::DutyCycle::kContinuousTempDeltaThresholdC,
      float humidityDeltaThresholdPct_ = SensingConfig::DutyCycle::kContinuousHumidityDeltaThresholdPct,
      bool failOnSampleError_ = SensingConfig::DutyCycle::kContinuousFailOnSampleError) {
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
                      ISensor **sensors, size_t sensorCount, IClock &clock, BatteryMonitor &battery);

  bool begin();
  void update();

  void markTelemetrySent();
  bool resetSensors();

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
  BatteryMonitor &_battery;
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
