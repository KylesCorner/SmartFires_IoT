#include "DroneApp.h"

#include <Arduino.h>
#include <Wire.h>
#include "esp32-hal-cpu.h"

DroneApp::DroneApp(uint8_t nodeId, DroneContext &ctx, AppState &state,
                   IClock &clock, SensorManager &sensors,
                   ActuatorManager &actuators, TelemetryService &telemetry,
                   LinkService &link, KeypadController &keypad,
                   OledPageController &oled)
    : _nodeId(nodeId), _ctx(ctx), _state(state), _clock(clock),
      _sensors(sensors), _actuators(actuators), _telemetry(telemetry),
      _link(link), _keypad(keypad), _oled(oled) {}

void DroneApp::setup() {
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(200);
  Wire.begin();
  Wire.setClock(100000); // SPS30 uses standard-mode I2C
  Wire.setTimeOut(50);   // milliseconds

  _sensors.beginAll();
  _actuators.beginAll();
  _ctx.bridge.begin();

  delay(200);

  // _sensors.sleepAllSensors();
  _sensors.startWakeSequence();
}

void DroneApp::loop() {
  _link.update();

  if (_state.sensorsSleeping != _state.lastSensorsSleeping) {
    if (_state.sensorsSleeping) {
      _sensors.sleepAllSensors();
    } else {
      _state.wakeupSequenceActive = true;
      _sensors.startWakeSequence();
    }

    _state.lastSensorsSleeping = _state.sensorsSleeping;
  }

  if (_state.sensorsSleeping) {
    _sensors.sampleKeypadOnly();
  } else if (_sensors.wakeSequenceActive()) {
    _sensors.serviceWakeSequence();
    _sensors.sampleKeypadOnly();
  } else {
    if (_state.sensingEnabled && !_state.sensorsSleeping) {
      _sensors.sampleAll();
    } else {
      _sensors.sampleKeypadOnly();
    }
    _state.wakeupSequenceActive = false;
  }

  if (_state.sensingEnabled && !_state.sensorsSleeping && !_sensors.wakeSequenceActive()) {
    _link.maybeSendTelemetry(_nodeId);
    _link.handleAckTimeout();
  }

  _actuators.updateAll();
  _keypad.update();

  if (_ctx.oled.healthy()) {
    _oled.render();
  }
  
}

void DroneApp::scanI2C() {
  Serial.println("I2C scan...");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("Found device at 0x");
      if (addr < 16)
        Serial.print('0');
      Serial.println(addr, HEX);
    }
  }
}
