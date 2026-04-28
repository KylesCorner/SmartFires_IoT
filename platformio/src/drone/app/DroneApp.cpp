#include "DroneApp.h"

#include "USBCDC.h"
#include "esp32-hal-cpu.h"
#include "esp32-hal.h"
#include <Arduino.h>
#include <Wire.h>
#include "../config/DutyCycleConfig.h"

DroneApp::DroneApp(uint8_t nodeId, DroneContext &ctx, AppState &state,
                   IClock &clock, SensorManager &sensors,
                   ActuatorManager &actuators, TelemetryService &telemetry,
                   LinkService &link, KeypadController &keypad,
                   OledPageController &oled, DutyCycleController &duty_cycle)
    : _nodeId(nodeId), _ctx(ctx), _state(state), _clock(clock),
      _sensors(sensors), _actuators(actuators), _telemetry(telemetry),
      _link(link), _keypad(keypad), _oled(oled), _duty_cycle(duty_cycle) {}

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

  delay(1000);

  _duty_cycle.begin(millis());
  _sensors.startWakeSequence();
}
void DroneApp::loop() {
  const uint32_t now = millis();

  _link.update();

  static bool lastContinuousMode = false;

  // Detect mode changes
  if (_state.continousMode != lastContinuousMode) {
    Serial.println(_state.continousMode
                       ? "[Mode] entering continuous mode"
                       : "[Mode] entering duty-cycle mode");

    _state.sensorsSleeping = false;
    _state.wakeupSequenceActive = true;
    _state.lastSensorsSleeping = false;

    _duty_cycle.begin(now);
    _sensors.startWakeSequence();

    lastContinuousMode = _state.continousMode;
  }

  // ============================================================
  // CONTINUOUS MODE
  // ============================================================
  if (_state.continousMode) {
    _duty_cycle.update_enabled(false);

    _state.sensorsSleeping = false;

    const bool sensorsActive =
        !_sensors.wakeSequenceActive() && _sensors.wakeSequenceComplete();

    if (!sensorsActive) {
      _state.wakeupSequenceActive = true;

      // Keep warming / waking sensors until complete
      _sensors.serviceWakeSequence();
      _sensors.sampleKeypadOnly();
    } else {
      _state.wakeupSequenceActive = false;

      if (_state.sensingEnabled) {
        _sensors.sampleAll();
      } else {
        _sensors.sampleKeypadOnly();
      }

      // Telemetry only when fully awake / active
      if (_state.sensingEnabled) {
        _link.maybeSendTelemetry(_nodeId);
        _link.handleAckTimeout();
      }
    }

    _actuators.updateAll();
    _keypad.update();

    if (_ctx.oled.healthy()) {
      _oled.render();
    }

    return;
  }

  // ============================================================
  // DUTY-CYCLE MODE
  // ============================================================
  _duty_cycle.update_enabled(true);

  DutyCycleController::Inputs dutyInputs;
  dutyInputs.wakeTrigger =
      _state.sensorsSleeping &&
      _sensors.sht31DeltaTriggered(TEMPERATURE_C_THRESHOLD, HUMIDITY_THRESHOLD);
  dutyInputs.wakeSequenceComplete = _sensors.wakeSequenceComplete();
  dutyInputs.sensingEnabled = _state.sensingEnabled;

  DutyCycleController::Actions dutyActions =
      _duty_cycle.update(now, dutyInputs);

  _state.sensorsSleeping = dutyActions.sensorsSleeping;
  _state.wakeupSequenceActive = dutyActions.wakeupSequenceActive;

  if (dutyActions.startWakeSequence) {
    Serial.println("[DutyCycle] start wake sequence");
    _sensors.startWakeSequence();
  }

  if (dutyActions.serviceWakeSequence) {
    _sensors.serviceWakeSequence();
  }

  if (dutyActions.sleepSensors) {
    Serial.println("[DutyCycle] sleep sensors");
    _sensors.sleepAllSensors();
  }

  if (dutyActions.sampleAll) {
    _sensors.sampleAll();
  }

  if (dutyActions.sampleKeypadOnly) {
    _sensors.sampleKeypadOnly();
  }

  // Telemetry only when the duty-cycle controller says we are awake
  if (dutyActions.sendTelemetry) {
    _link.maybeSendTelemetry(_nodeId);
    _link.maybeSendGpsOnce(_nodeId);
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
