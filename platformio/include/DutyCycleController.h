#pragma once

#include <Arduino.h>

class DutyCycleController {
public:
  struct Config {
    uint32_t awakeMs;
    uint32_t minSleepMs;
    bool enabled;

    Config();
    Config(uint32_t awakeMs_, uint32_t minSleepMs_, bool enabled_);
  };

  struct Inputs {
    bool wakeTrigger = false;
    bool wakeSequenceComplete = false;
    bool sensingEnabled = true;
  };

  struct Actions {
    bool startWakeSequence = false;
    bool serviceWakeSequence = false;
    bool sleepSensors = false;
    bool sampleAll = false;
    bool sampleKeypadOnly = false;

    bool sensorsSleeping = false;
    bool wakeupSequenceActive = false;
    bool sendTelemetry = false;
  };

  explicit DutyCycleController(const Config &cfg);

  void begin(uint32_t nowMs);
  Actions update(uint32_t nowMs, const Inputs &inputs);
  void update_enabled(bool isEnabled);

private:
  enum class Phase {
    Waking,
    Awake,
    Sleeping
  };

  Config _cfg;
  Phase _phase = Phase::Waking;
  uint32_t _phaseStartMs = 0;
  bool _wakeSequenceStarted = false;

  void transitionTo_(Phase next, uint32_t nowMs);
  bool phaseElapsed_(uint32_t nowMs, uint32_t durationMs) const;
};
