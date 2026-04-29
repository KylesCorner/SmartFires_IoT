#include "sensors/Pa1010dGpsSensor.h"

#include <stdio.h>

Pa1010dGpsSensor::Pa1010dGpsSensor(const Config &cfg, IGpsDriver &driver,
                                   IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Pa1010dGpsSensor::name() const { return "gps"; }

bool Pa1010dGpsSensor::begin() {
  _healthy = _driver.begin(_cfg.address);
  _state = _healthy ? SensorPowerState::Ready : SensorPowerState::Error;
  _lastSampleMs = 0;
  return _healthy;
}

bool Pa1010dGpsSensor::wake() {
  if (!_healthy) return false;

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn) {
    _state = SensorPowerState::Ready;
    return true;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;
  return true;
}

bool Pa1010dGpsSensor::sleep() {
  if (!_healthy) return false;

  _state = (_cfg.dutyClass == SensorDutyClass::AlwaysOn)
               ? SensorPowerState::Ready
               : SensorPowerState::Sleeping;

  return true;
}

bool Pa1010dGpsSensor::service() {
  if (!_healthy) return false;

  if (!_driver.poll()) {
    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    _state = SensorPowerState::Ready;
  }

  return _state == SensorPowerState::Ready;
}

bool Pa1010dGpsSensor::sample() {
  if (!ready()) return false;

  IGpsDriver::Data data;
  if (!_driver.read(data)) {
    _reading.valid = false;
    return false;
  }

  static_cast<IGpsDriver::Data &>(_reading) = data;

  _reading.valid = data.fix;
  _reading.timestampMs = _clock.millis();
  _lastSampleMs = _clock.millis();

  return true;
}

bool Pa1010dGpsSensor::ready() const {
  return _healthy && _state == SensorPowerState::Ready &&
         _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Pa1010dGpsSensor::healthy() const { return _healthy; }

SensorPowerState Pa1010dGpsSensor::powerState() const { return _state; }

SensorDutyClass Pa1010dGpsSensor::dutyClass() const { return _cfg.dutyClass; }

const Pa1010dGpsSensor::Reading &Pa1010dGpsSensor::reading() const {
  return _reading;
}

const void *Pa1010dGpsSensor::readingData() const { return &_reading; }

size_t Pa1010dGpsSensor::readingSize() const { return sizeof(Reading); }

size_t Pa1010dGpsSensor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) return 0;

  int n = snprintf(
      out, maxLen,
      "gps,fix=%u,fixq=%u,sats=%u,lat=%.6f,lon=%.6f,alt=%.2f,t=%02u:%02u:%02u,valid=%u,t_ms=%lu",
      _reading.fix ? 1 : 0,
      _reading.fixQuality,
      _reading.satellites,
      _reading.latitudeDeg,
      _reading.longitudeDeg,
      _reading.altitudeM,
      _reading.hour,
      _reading.minute,
      _reading.second,
      _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) return 0;
  return static_cast<size_t>(n) >= maxLen ? maxLen - 1 : static_cast<size_t>(n);
}
