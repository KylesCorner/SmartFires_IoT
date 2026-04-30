#include "sensors/Sht31Sensor.h"
#include <Arduino.h>
#include <stdio.h>

Sht31Sensor::Sht31Sensor(const Config &cfg, ISht31Driver &driver, IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Sht31Sensor::name() const { return "sht31"; }

bool Sht31Sensor::begin() {
  _healthy = _driver.begin(_cfg.address);
  if (!_healthy) {
    Serial.println("SHT31 is not healthy!");

    _state = SensorPowerState::Error;
    return false;
  }

  _state = (_cfg.dutyClass == SensorDutyClass::AlwaysOn)
               ? SensorPowerState::Ready
               : SensorPowerState::Sleeping;

  return true;
}

bool Sht31Sensor::wake() {
  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;
  return true;
}

bool Sht31Sensor::sleep() {
  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  _state = (_cfg.dutyClass == SensorDutyClass::AlwaysOn)
               ? SensorPowerState::Ready
               : SensorPowerState::Sleeping;

  return true;
}

bool Sht31Sensor::service() {
  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    _state = SensorPowerState::Ready;
  }

  return _state == SensorPowerState::Ready;
}

bool Sht31Sensor::sample() {
  if (!ready()) {
    return false;
  }

  const float tempC = _driver.readTemperatureC();
  const float humidityPct = _driver.readHumidityPct();

  _reading.tempC = tempC;
  _reading.humidityPct = humidityPct;
  _reading.valid = !isnan(tempC) && !isnan(humidityPct);
  _reading.timestampMs = _clock.millis();

  _triggerReading.valid = _reading.valid;
  _triggerReading.tempC = _reading.tempC;
  _triggerReading.humidityPct = _reading.humidityPct;

  _lastSampleMs = _clock.millis();

  if (!_reading.valid) {
    _healthy = false;
    _state = SensorPowerState::Error;
    return false;
  }

  return true;
}
const ITriggerSensor::Reading &Sht31Sensor::triggerReading() const {
  return _triggerReading;
}

bool Sht31Sensor::ready() const {
  if (!_healthy || _state != SensorPowerState::Ready) {
    return false;
  }

  return _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Sht31Sensor::healthy() const { return _healthy; }

SensorPowerState Sht31Sensor::powerState() const { return _state; }

SensorDutyClass Sht31Sensor::dutyClass() const { return _cfg.dutyClass; }

const Sht31Sensor::Reading &Sht31Sensor::reading() const { return _reading; }

const void *Sht31Sensor::readingData() const { return &_reading; }

size_t Sht31Sensor::readingSize() const { return sizeof(Reading); }

void Sht31Sensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid)
    return;
  snap.tempC = _reading.tempC;
  snap.humidityPct = _reading.humidityPct;
  snap.sensorFlags |= 0x02; // SHT31
}

size_t Sht31Sensor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) {
    return 0;
  }

  const int n = snprintf(
      out, maxLen, "sht31,temp_c=%.2f,humidity_pct=%.2f,valid=%u,t_ms=%lu",
      _reading.tempC, _reading.humidityPct, _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) {
    return 0;
  }

  if (static_cast<size_t>(n) >= maxLen) {
    return maxLen - 1;
  }

  return static_cast<size_t>(n);
}
