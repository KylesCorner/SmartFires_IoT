#include "sensors/Pa1010dGpsSensor.h"

#include <stdio.h>

Pa1010dGpsSensor::Pa1010dGpsSensor(const Config &cfg, IGpsDriver &driver,
                                   IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Pa1010dGpsSensor::name() const { return "gps"; }

bool Pa1010dGpsSensor::begin() {
  _healthy = _driver.begin(_cfg.address);

  if (!_healthy) {
    _state = SensorPowerState::Error;
    _lastSampleMs = 0;
    return false;
  }

  bool ok = true;

  switch (_cfg.powerMode) {
  case GpsPowerMode::FullPowerContinuous:
    ok = _driver.enterFullPower();
    break;

  case GpsPowerMode::Standby:
  case GpsPowerMode::Backup:
    // Do not immediately sleep during begin().
    // These are controlled by the app-level duty-cycle controller.
    ok = true;
    break;

  case GpsPowerMode::PeriodicStandby:
    ok = _driver.enterPeriodicStandby(_cfg.periodic);
    break;

  case GpsPowerMode::PeriodicBackup:
    ok = _driver.enterPeriodicBackup(_cfg.periodic);
    break;
  }

  if (!ok) {
    _healthy = false;
    _state = SensorPowerState::Error;
    _lastSampleMs = 0;
    return false;
  }

  _state = SensorPowerState::Ready;
  _lastSampleMs = 0;
  return true;
}

bool Pa1010dGpsSensor::wake() {
  if (!_healthy) return false;

  bool ok = true;

  switch (_cfg.powerMode) {
  case GpsPowerMode::FullPowerContinuous:
    ok = _driver.enterFullPower();
    break;

  case GpsPowerMode::Standby:
    ok = _driver.enterFullPower();
    break;

  case GpsPowerMode::Backup:
    ok = _driver.wakeFromBackup();
    break;

  case GpsPowerMode::PeriodicStandby:
  case GpsPowerMode::PeriodicBackup:
    // Periodic mode is autonomous. Do not send PMTK225,0 here,
    // because that exits periodic mode.
    ok = true;
    break;
  }

  if (!ok) {
    _state = SensorPowerState::Error;
    _healthy = false;
    return false;
  }

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn ||
      _cfg.powerMode == GpsPowerMode::PeriodicStandby ||
      _cfg.powerMode == GpsPowerMode::PeriodicBackup) {
    _state = SensorPowerState::Ready;
    return true;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;
  return true;
}

bool Pa1010dGpsSensor::sleep() {
  if (!_healthy) return false;

  switch (_cfg.powerMode) {
  case GpsPowerMode::FullPowerContinuous:
    _state = SensorPowerState::Ready;
    return true;

  case GpsPowerMode::Standby:
    if (!_driver.enterStandby()) {
      _state = SensorPowerState::Error;
      _healthy = false;
      return false;
    }
    _state = SensorPowerState::Sleeping;
    return true;

  case GpsPowerMode::Backup:
    if (!_driver.enterBackup()) {
      _state = SensorPowerState::Error;
      _healthy = false;
      return false;
    }
    _state = SensorPowerState::Sleeping;
    return true;

  case GpsPowerMode::PeriodicStandby:
  case GpsPowerMode::PeriodicBackup:
    // Already handled internally by PA1010D.
    // Keep the software sensor ready so service()/poll() continues.
    _state = SensorPowerState::Ready;
    return true;
  }

  return false;
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

void Pa1010dGpsSensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    return;
  }

  snap.latDeg = _reading.latitudeDeg;
  snap.lonDeg = _reading.longitudeDeg;
  snap.sensorFlags |= 0x04; // GPS
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
