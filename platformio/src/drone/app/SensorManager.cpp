#include "SensorManager.h"
#include "ISensor.h"
#include "IWarmup.h"
#include "USBCDC.h"
#include <Arduino.h>

void SensorManager::beginAll() {
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    const bool ok = _ctx.sensors[i]->begin();
    Serial.print("Begin ");
    Serial.print(_ctx.sensors[i]->name());
    Serial.print(": ");
    Serial.println(ok ? "OK" : "FAIL");
  }
}

void SensorManager::sampleAll() {
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    _ctx.sensors[i]->sample();
  }
}

void SensorManager::sampleKeypadOnly() {
  _ctx.keypad.sample();
}

void SensorManager::sleepAllSensors() {
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    const bool ok = _ctx.sensors[i]->sleep();
    Serial.print("Sleep ");
    Serial.print(_ctx.sensors[i]->name());
    Serial.print(": ");
    Serial.println(ok ? "OK" : "FAIL");
  }

  _wakePhase = WakePhase::Idle;
}

bool SensorManager::isPriorityWarmupSensor_(size_t index) const {
  const IWarmup* warm = _ctx.sensors[index]->warmup();
  return (warm != nullptr) && warm->requiresPriorityWarmup();
}

bool SensorManager::wakeSensor_(size_t index, const char* label) {
  const bool ok = _ctx.sensors[index]->wake();
  Serial.print(label);
  Serial.print(_ctx.sensors[index]->name());
  Serial.print(": ");
  Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

void SensorManager::startWakeSequence() {
  // Phase 1: only wake priority warmup sensors.
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    if (isPriorityWarmupSensor_(i)) {
      wakeSensor_(i, "Wake priority ");
    }
  }

  _wakePhase = WakePhase::WaitingPriorityWarmup;
}

void SensorManager::serviceWakeSequence() {
  // Keep already-awake sensors progressing.
  sampleAll();

  switch (_wakePhase) {
    case WakePhase::Idle:
    case WakePhase::Complete:
      return;

    case WakePhase::WaitingPriorityWarmup:
      if (!priorityWarmupComplete()) {
        return;
      }

      // Phase 2: wake everything else.
      for (size_t i = 0; i < _ctx.numSensors; ++i) {
        if (!isPriorityWarmupSensor_(i)) {
          wakeSensor_(i, "Wake remaining ");
        }
      }

      _wakePhase = WakePhase::WaitingAllReady;
      return;

    case WakePhase::WaitingAllReady:
      if (allSensorsReady()) {
        _wakePhase = WakePhase::Complete;
      }
      return;
  }
}

bool SensorManager::priorityWarmupComplete() const {
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    const IWarmup* warm = _ctx.sensors[i]->warmup();
    if (warm != nullptr && warm->requiresPriorityWarmup()) {
      if (!_ctx.sensors[i]->ready()) {
        return false;
      }
    }
  }
  return true;
}

bool SensorManager::allSensorsReady() const {
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    if (!_ctx.sensors[i]->ready()) {
      return false;
    }
  }
  return true;
}

bool SensorManager::wakeSequenceActive() const {
  return _wakePhase != WakePhase::Idle &&
         _wakePhase != WakePhase::Complete;
}

bool SensorManager::wakeSequenceComplete() const {
  return _wakePhase == WakePhase::Complete;
}

void SensorManager::printNotReadySensors() const {
  Serial.println("Sensors not ready:");
  bool any = false;

  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    if (!_ctx.sensors[i]->ready()) {
      Serial.print(" - ");
      Serial.println(_ctx.sensors[i]->name());
      any = true;
    }
  }

  if (!any) {
    Serial.println(" - none");
  }
}

void SensorManager::printSensorReadings() {
  Serial.println(" === Sensor Readings === ");
  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    if (_ctx.sensors[i]->hasReading()) {
      Serial.printf("Name: %s has a reading\n", _ctx.sensors[i]->name());
    } else {
      Serial.printf("Name: %s no readings\n", _ctx.sensors[i]->name());
    }
  }
  Serial.println(" =======================");
}
