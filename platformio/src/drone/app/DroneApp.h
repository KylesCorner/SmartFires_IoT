#pragma once

#include <stdint.h>
#include "DroneContext.h"
#include "DroneState.h"
#include "IClock.h"
#include "SensorManager.h"
#include "ActuatorManager.h"
#include "TelemetryService.h"
#include "LinkService.h"
#include "KeypadController.h"
#include "OledPageController.h"

class DroneApp {
public:
  DroneApp(uint8_t nodeId,
           DroneContext& ctx,
           AppState& state,
           IClock& clock,
           SensorManager& sensors,
           ActuatorManager& actuators,
           TelemetryService& telemetry,
           LinkService& link,
           KeypadController& keypad,
           OledPageController& oled);

  void setup();
  void loop();
  void scanI2C();

private:
  uint8_t _nodeId;
  DroneContext& _ctx;
  AppState& _state;
  IClock& _clock;
  SensorManager& _sensors;
  ActuatorManager& _actuators;
  TelemetryService& _telemetry;
  LinkService& _link;
  KeypadController& _keypad;
  OledPageController& _oled;

  bool _sensorsSleeping = false;
};
