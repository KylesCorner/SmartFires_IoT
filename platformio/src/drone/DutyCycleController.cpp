#include "DutyCycleController.h"

DutyCycleController::Config::Config()
    : awakeMs(30000),
      minSleepMs(60000),
      enabled(true) {}

DutyCycleController::Config::Config(uint32_t awakeMs_, uint32_t minSleepMs_,
                                     bool enabled_)
    : awakeMs(awakeMs_),
      minSleepMs(minSleepMs_),
      enabled(enabled_) {}

DutyCycleController::DutyCycleController(const Config &cfg)
    : _cfg(cfg) {}

void DutyCycleController::begin(uint32_t nowMs) {
  _phase = Phase::Waking;
  _phaseStartMs = nowMs;
  _wakeSequenceStarted = false;
}

DutyCycleController::Actions
DutyCycleController::update(uint32_t nowMs, const Inputs &inputs) {
  Actions actions;

  if (!_cfg.enabled) {
    _phase = Phase::Awake;
    actions.sensorsSleeping = false;
    actions.wakeupSequenceActive = false;
    actions.sampleAll = inputs.sensingEnabled;
    actions.sampleKeypadOnly = !inputs.sensingEnabled;
    return actions;
  }

  switch (_phase) {
  case Phase::Waking:
    actions.sensorsSleeping = false;
    actions.wakeupSequenceActive = true;
    actions.serviceWakeSequence = true;
    actions.sampleKeypadOnly = true;

    if (!_wakeSequenceStarted) {
      actions.startWakeSequence = true;
      _wakeSequenceStarted = true;
    }

    if (inputs.wakeSequenceComplete) {
      transitionTo_(Phase::Awake, nowMs);

      actions.wakeupSequenceActive = false;
      actions.serviceWakeSequence = false;
      actions.sampleKeypadOnly = false;
    }
    break;

  case Phase::Awake:
    actions.sensorsSleeping = false;
    actions.wakeupSequenceActive = false;
    actions.sendTelemetry  = true;

    if (inputs.sensingEnabled) {
      actions.sampleAll = true;
    } else {
      actions.sampleKeypadOnly = true;
    }

    if (phaseElapsed_(nowMs, _cfg.awakeMs)) {
      transitionTo_(Phase::Sleeping, nowMs);

      actions.sampleAll = false;
      actions.sampleKeypadOnly = true;
      actions.sleepSensors = true;
      actions.sensorsSleeping = true;
    }
    break;

  case Phase::Sleeping:
    actions.sensorsSleeping = true;
    actions.wakeupSequenceActive = false;
    actions.sampleKeypadOnly = true;
    actions.sendTelemetry = false;

    if (phaseElapsed_(nowMs, _cfg.minSleepMs) && inputs.wakeTrigger) {
      transitionTo_(Phase::Waking, nowMs);

      actions.sensorsSleeping = false;
      actions.wakeupSequenceActive = true;
      actions.sampleKeypadOnly = false;
      actions.startWakeSequence = true;
      actions.serviceWakeSequence = true;
      _wakeSequenceStarted = true;
    }
    break;
  }

  return actions;
}

void DutyCycleController::transitionTo_(Phase next, uint32_t nowMs) {
  _phase = next;
  _phaseStartMs = nowMs;

  if (next == Phase::Waking) {
    _wakeSequenceStarted = false;
  }
}

bool DutyCycleController::phaseElapsed_(uint32_t nowMs,
                                        uint32_t durationMs) const {
  return nowMs - _phaseStartMs >= durationMs;
}

void DutyCycleController::update_enabled(bool isEnabled){
  _cfg.enabled = isEnabled;
}
