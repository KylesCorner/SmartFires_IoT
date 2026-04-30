#include "sensors/Sps30Sensor.h"

#include <stdio.h>

Sps30Sensor::Sps30Sensor(const Config &cfg, ISps30Driver &driver,
                         IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Sps30Sensor::name() const { return "sps30"; }

bool Sps30Sensor::begin() {
  _healthy = _driver.begin();

  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  _state = SensorPowerState::Sleeping;
  return true;
}

bool Sps30Sensor::wake() {
  if (!_healthy) {
    return false;
  }

  if (_state == SensorPowerState::Ready ||
      _state == SensorPowerState::Waking) {
    return true;
  }

  if (!_driver.startMeasurement()) {
    _state = SensorPowerState::Error;
    _healthy = false;
    return false;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;
  return true;
}

bool Sps30Sensor::sleep() {
  if (!_healthy) {
    return false;
  }

  if (_state == SensorPowerState::Sleeping) {
    return true;
  }

  if (!_driver.stopMeasurement()) {
    _state = SensorPowerState::Error;
    _healthy = false;
    return false;
  }

  _state = SensorPowerState::Sleeping;
  return true;
}

bool Sps30Sensor::service() {
  if (!_healthy) {
    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    _state = SensorPowerState::Ready;
  }

  return _state == SensorPowerState::Ready;
}

bool Sps30Sensor::sample() {
  if (!ready()) {
    return false;
  }

  ISps30Driver::Data data;

  if (!_driver.read(data) || !data.valid) {
    _reading.valid = false;
    return false;
  }

  const uint32_t now = _clock.millis();

  _reading.pm1_0 = data.pm1_0;
  _reading.pm2_5 = data.pm2_5;
  _reading.pm4_0 = data.pm4_0;
  _reading.pm10_0 = data.pm10_0;
  _reading.valid = true;
  _reading.timestampMs = now;

  _lastSampleMs = now;

  return true;
}

bool Sps30Sensor::ready() const {
  return _healthy && _state == SensorPowerState::Ready &&
         _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Sps30Sensor::healthy() const { return _healthy; }

SensorPowerState Sps30Sensor::powerState() const { return _state; }

SensorDutyClass Sps30Sensor::dutyClass() const { return _cfg.dutyClass; }

const Sps30Sensor::Reading &Sps30Sensor::reading() const { return _reading; }

const void *Sps30Sensor::readingData() const { return &_reading; }

size_t Sps30Sensor::readingSize() const { return sizeof(Reading); }

size_t Sps30Sensor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) {
    return 0;
  }

  const int n = snprintf(
      out, maxLen,
      "sps30,pm1=%.2f,pm25=%.2f,pm4=%.2f,pm10=%.2f,valid=%u,t_ms=%lu",
      _reading.pm1_0, _reading.pm2_5, _reading.pm4_0, _reading.pm10_0,
      _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) {
    return 0;
  }

  return static_cast<size_t>(n) >= maxLen ? maxLen - 1
                                          : static_cast<size_t>(n);
}

void Sps30Sensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    return;
  }

  snap.pm1_0 = _reading.pm1_0;
  snap.pm2_5 = _reading.pm2_5;
  snap.pm4_0 = _reading.pm4_0;
  snap.pm10 = _reading.pm10_0;

  snap.sensorFlags |= 0x10;
}
