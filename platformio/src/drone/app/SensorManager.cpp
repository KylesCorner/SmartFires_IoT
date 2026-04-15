#include "SensorManager.h"
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
}

void SensorManager::wakeAllSensors() {

  for (size_t i = 0; i < _ctx.numSensors; ++i) {
    const bool ok = _ctx.sensors[i]->wake();
    Serial.print("Wake ");
    Serial.print(_ctx.sensors[i]->name());
    Serial.print(": ");
    Serial.println(ok ? "OK" : "FAIL");
  }
}
