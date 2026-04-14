#include "ActuatorManager.h"

#include <Arduino.h>

ActuatorManager::ActuatorManager(DroneContext& ctx) : _ctx(ctx) {}

void ActuatorManager::beginAll() {
  for (size_t i = 0; i < _ctx.numActuators; ++i) {
    if (_ctx.actuators[i] == nullptr) {
      Serial.println("Begin actuator: NULL");
      continue;
    }

    const bool ok = _ctx.actuators[i]->begin();

    Serial.print("Begin ");
    Serial.print(_ctx.actuators[i]->name());
    Serial.print(": ");
    Serial.println(ok ? "OK" : "FAIL");
  }
}

void ActuatorManager::updateAll() {
  for (size_t i = 0; i < _ctx.numActuators; ++i) {
    if (_ctx.actuators[i] == nullptr) {
      continue;
    }

    _ctx.actuators[i]->update();
  }
}

