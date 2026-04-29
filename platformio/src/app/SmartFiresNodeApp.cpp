#include "app/SmartFiresNodeApp.h"
#include <Arduino.h>

SmartFiresNodeApp::SmartFiresNodeApp(
    const Config &cfg,
    IClock &clock,
    DutyCycleController &duty,
    TelemetryBuilder &telemetry,
    TdmaRadioService &radio,
    ISensor **sensors,
    size_t sensorCount,
    BatteryMonitor *battery)
    : _cfg(cfg),
      _clock(clock),
      _duty(duty),
      _telemetry(telemetry),
      _radio(radio),
      _sensors(sensors),
      _sensorCount(sensorCount),
      _battery(battery) {}

bool SmartFiresNodeApp::begin() {
  if (_cfg.enableBattery && _battery) {
    if (!_battery->begin()) {
      Serial.println("Battery Failed to begin!");
      return false;
    }
  }

  if (!_duty.begin()) {
    Serial.println("Duty Failed to begin!");
    return false;
  }

  if (!_radio.begin()) {
    Serial.println("Radio Failed to begin!");
    return false;
  }

  _initialized = true;
  return true;
}
void SmartFiresNodeApp::update() {
  if (!_initialized) {
    return;
  }

  // Keep LoRa/TDMA alive every loop.
  _radio.update();

  // Advance duty-cycle state machine.
  _duty.update();

  // Battery is sampled independently from duty-cycled sensors.
  if (_cfg.enableBattery && _battery && _battery->ready()) {
    _battery->sample();
  }

  // When duty cycle says readings are ready, build telemetry and queue it
  // for TDMA-gated LoRa TX.
  if (_duty.telemetryReady()) {
    TelemetryFrame frame;

    if (_telemetry.build(frame, _sensors, _sensorCount, _battery)) {
      _radio.enqueueTelemetry(
          reinterpret_cast<const uint8_t *>(frame.payload),
          static_cast<uint8_t>(frame.len));
    }

    _duty.markTelemetrySent();
  }
}
