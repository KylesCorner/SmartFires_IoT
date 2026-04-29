#include "app/SmartFiresNodeApp.h"

SmartFiresNodeApp::SmartFiresNodeApp(
    const Config &cfg,
    IClock &clock,
    DutyCycleController &duty,
    TelemetryBuilder &telemetry,
    RadioService &radio,
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
      return false;
    }
  }

  if (!_duty.begin()) {
    return false;
  }

  if (!_radio.begin()) {
    return false;
  }

  _initialized = true;
  return true;
}

void SmartFiresNodeApp::update() {
  if (!_initialized) {
    return;
  }

  // --- Always update radio (receive + ACK handling)
  _radio.update();

  // --- Duty cycle progression
  _duty.update();

  // --- Battery sampling (independent)
  if (_cfg.enableBattery && _battery && _battery->ready()) {
    _battery->sample();
  }

  // --- Telemetry phase
  if (_duty.telemetryReady()) {
    TelemetryFrame frame;

    if (_telemetry.build(frame,
                         _sensors,
                         _sensorCount,
                         _battery)) {
      _radio.sendTelemetry(frame);
    }

    _duty.markTelemetrySent();
  }
}
