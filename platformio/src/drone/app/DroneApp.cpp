#include "DroneApp.h"

#include <Arduino.h>
#include <Wire.h>

DroneApp::DroneApp(uint8_t nodeId, DroneContext &ctx, AppState &state,
                   IClock &clock, SensorManager &sensors,
                   ActuatorManager &actuators, TelemetryService &telemetry,
                   LinkService &link, KeypadController &keypad,
                   OledPageController &oled)
    : _nodeId(nodeId), _ctx(ctx), _state(state), _clock(clock),
      _sensors(sensors), _actuators(actuators), _telemetry(telemetry),
      _link(link), _keypad(keypad), _oled(oled) {}

void DroneApp::setup() {
  Serial.begin(115200);
  delay(200);
  Wire.begin();

  _sensors.beginAll();
  _actuators.beginAll();
  _ctx.bridge.begin();
}
void DroneApp::loop() {
  _link.update();

  // Only perform sleep/wake action when the state changes.
  if (_state.sensorsSleeping != _state.lastSensorsSleeping) {
    if (_state.sensorsSleeping) {
      _sensors.sleepAllSensors();
    } else {
      _sensors.wakeAllSensors();
    }

    _state.lastSensorsSleeping = _state.sensorsSleeping;
  }

  // Do not sample sensors while they are sleeping.
  if (_state.sensorsSleeping) {
    _sensors.sampleKeypadOnly();
  } else {
    if (_state.sensingEnabled) {
      _sensors.sampleAll();
    } else {
      _sensors.sampleKeypadOnly();
    }
  }

  _actuators.updateAll();
  _keypad.update();
  _link.maybeSendTelemetry(_nodeId);
  _link.handleAckTimeout();

  if (_ctx.oled.healthy()) {
    _oled.render();
  }
}
