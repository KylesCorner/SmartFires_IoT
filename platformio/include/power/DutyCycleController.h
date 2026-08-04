// ---
// description: Sensor wake/sample/sleep duty-cycle state machine, gated by an ITriggerSensor threshold crossing and driving all ISensor instances plus the battery monitor.
// role: implementation
// docs: [duty-cycling]
// ---
#pragma once

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

enum class DutyCycleMode : uint8_t {
  Continuous,
  SensorTriggered,
  Timed,
  Hybrid
};

struct DutyCycleConfig {
  DutyCycleMode wakeMode;

  uint32_t minSleepMs;
  uint32_t maxWakeMs;
  uint32_t activeSampleMs;
  uint32_t samplePeriodMs;
  uint32_t warmupMs;
  uint32_t timedSleepMs;

  float tempDeltaThresholdC;
  float humidityDeltaThresholdPct;

  bool failOnSampleError;

  // Kept internally because the existing controller has a continuous-mode
  // path controlled by this flag.
  bool enabled;

  static DutyCycleConfig make(
      DutyCycleMode wakeMode_ =
          DutyCycleMode::SensorTriggered,
      uint32_t minSleepMs_ = 3000,
      uint32_t maxWakeMs_ = 1000,
      uint32_t activeSampleMs_ = 30000,
      uint32_t samplePeriodMs_ = 750,
      uint32_t warmupMs_ = 15000,
      uint32_t timedSleepMs_ = 0,
      float tempDeltaThresholdC_ = 1.0f,
      float humidityDeltaThresholdPct_ = 1.0f,
      bool failOnSampleError_ = false) {
    DutyCycleConfig cfg;

    cfg.wakeMode = wakeMode_;

    cfg.minSleepMs = minSleepMs_;
    cfg.maxWakeMs = maxWakeMs_;
    cfg.activeSampleMs = activeSampleMs_;
    cfg.samplePeriodMs = samplePeriodMs_;
    cfg.warmupMs = warmupMs_;
    cfg.timedSleepMs = timedSleepMs_;

    cfg.tempDeltaThresholdC =
        tempDeltaThresholdC_;
    cfg.humidityDeltaThresholdPct =
        humidityDeltaThresholdPct_;

    cfg.failOnSampleError =
        failOnSampleError_;

    cfg.enabled =
        wakeMode_ != DutyCycleMode::Continuous;

    return cfg;
  }
};
// struct DutyCycleConfig {
//   uint32_t minSleepMs;
//   uint32_t maxWakeMs;
//   uint32_t activeSampleMs;
//   uint32_t samplePeriodMs;
//   uint32_t warmupMs;

//   float tempDeltaThresholdC;
//   float humidityDeltaThresholdPct;
//   bool failOnSampleError;
//   bool enabled;

//   static DutyCycleConfig make(
//       bool enabled_ = true,
//       uint32_t minSleepMs_ = 3000,
//       uint32_t maxWakeMs_ = 1000,
//       uint32_t activeSampleMs_ = 30000,
//       uint32_t samplePeriodMs_ = 750,
//       uint32_t warmupMs_ = 15000,
//       float tempDeltaThresholdC_ = 1,
//       float humidityDeltaThresholdPct_ = 1,
//       bool failOnSampleError_ = false) {
//     DutyCycleConfig cfg;
//     cfg.minSleepMs = minSleepMs_;
//     cfg.maxWakeMs = maxWakeMs_;
//     cfg.failOnSampleError = failOnSampleError_;
//     cfg.activeSampleMs = activeSampleMs_;
//     cfg.samplePeriodMs = samplePeriodMs_;
//     cfg.warmupMs = warmupMs_;
//     cfg.tempDeltaThresholdC = tempDeltaThresholdC_;
//     cfg.humidityDeltaThresholdPct = humidityDeltaThresholdPct_;
//     cfg.failOnSampleError = failOnSampleError_;
//     cfg.enabled = enabled_;
//     return cfg;
//   }
// };

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

  DutyCycleMode mode() const;
  bool sleeping() const;
  uint32_t timedSleepRemainingMs() const;

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

  uint32_t _sleepStartMs = 0;
  bool _triggerLatched = false;

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

  bool triggerWakeEnabled() const;
  bool timedWakeEnabled() const;
  uint32_t sleepElapsedMs() const;
  void sampleSleepTrigger();
  bool wakeFromSleepIfNeeded();


};
