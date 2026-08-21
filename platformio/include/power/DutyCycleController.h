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

// Every field carries a default so that a partially-populated
// `DutyCycleConfig cfg;` (as tests build) leaves the rest zeroed rather than
// indeterminate. The zero defaults are all "feature off": no timed wakeup, no
// active-window hold.
struct DutyCycleConfig {
  DutyCycleMode wakeMode = DutyCycleMode::SensorTriggered;

  uint32_t minSleepMs = 0;
  uint32_t maxWakeMs = 0;
  uint32_t activeSampleMs = 0;
  uint32_t samplePeriodMs = 0;
  uint32_t warmupMs = 0;

  // Timed/Hybrid: fixed wake-to-wake period. The standby is the remainder once
  // warmup, the active window (plus any full-bundle overrun) and the post-close
  // TX drain have taken their share — so an overrun shortens the sleep instead
  // of stretching the cycle. Floored at minStandbyMs. 0 disables timed wakeup.
  uint32_t cyclePeriodMs = 0;
  uint32_t minStandbyMs = 0;

  // How far past activeSampleMs the active window may be held open waiting for
  // the sample accumulator to complete its bundle (setActiveWindowHold()).
  // 0 disables the hold, closing the window strictly on activeSampleMs.
  uint32_t activeOverrunMaxMs = 0;

  float tempDeltaThresholdC = 0.0f;
  float humidityDeltaThresholdPct = 0.0f;

  bool failOnSampleError = false;

  // Kept internally because the existing controller has a continuous-mode
  // path controlled by this flag.
  bool enabled = true;

  static DutyCycleConfig make(
      DutyCycleMode wakeMode_ =
          DutyCycleMode::SensorTriggered,
      uint32_t minSleepMs_ = 3000,
      uint32_t maxWakeMs_ = 1000,
      uint32_t activeSampleMs_ = 30000,
      uint32_t samplePeriodMs_ = 750,
      uint32_t warmupMs_ = 15000,
      uint32_t cyclePeriodMs_ = 0,
      uint32_t minStandbyMs_ = 0,
      uint32_t activeOverrunMaxMs_ = 0,
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
    cfg.cyclePeriodMs = cyclePeriodMs_;
    cfg.minStandbyMs = minStandbyMs_;
    cfg.activeOverrunMaxMs = activeOverrunMaxMs_;

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

  // --- Full-bundle active window ---
  //
  // Set from PacketHandler::hasPartialBundle() *before* each update(), so the
  // window never closes mid-bundle. Once activeSampleMs has elapsed the window
  // stays open while this is true, up to cfg.activeOverrunMaxMs past it; past
  // that cap it closes anyway and the caller force-encodes the runt.
  //
  // Sizing activeSampleMs as a whole number of bundles (SensingConfig's
  // static_assert) means the accumulator is normally empty exactly when the
  // window expires, so the hold costs nothing.
  void setActiveWindowHold(bool hold);

  // Set at window close, for PKT_WINDOW_END's payload. plannedSleepMs is the
  // standby the fixed-period arithmetic arrived at; sampleCount is how many
  // samples the window just closed actually produced (a direct check that the
  // window really did land on a bundle boundary).
  uint32_t plannedSleepMs() const;
  uint16_t lastWindowSampleCount() const;
  bool lastWindowOverran() const;

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

  // Start of the current wake-to-wake cycle (the moment sleep ended). The fixed
  // period is measured from here, so warmup jitter, window overrun and the
  // post-close TX drain all come out of the standby rather than stretching the
  // cycle.
  uint32_t _cycleStartMs = 0;

  bool _activeWindowHold = false;
  uint16_t _activeSampleCount = 0;
  uint16_t _lastWindowSampleCount = 0;
  bool _lastWindowOverran = false;
  uint32_t _plannedSleepMs = 0;

  void transitionTo(DutyCyclePhase next);
  bool activeWindowShouldClose() const;
  uint32_t computePlannedSleepMs() const;

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
