#include "sensors/Icm20948Sensor.h"
#include <stdio.h>

namespace {

int16_t clampToInt16(float value) {
  if (value > 32767.0f) {
    return 32767;
  }
  if (value < -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(value);
}

} // namespace

Icm20948Sensor::Icm20948Sensor(const Config &cfg, IIcm20948Driver &driver,
                               IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Icm20948Sensor::name() const { return "imu"; }

bool Icm20948Sensor::begin() {
  _healthy = _driver.begin(_cfg.address);
  _state = _healthy ? SensorPowerState::Sleeping : SensorPowerState::Error;
  return _healthy;
}

bool Icm20948Sensor::wake() {
  if (!_healthy) return false;
  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;
  return true;
}

bool Icm20948Sensor::sleep() {
  if (!_healthy) return false;
  _state = SensorPowerState::Sleeping;
  return true;
}

bool Icm20948Sensor::service() {
  if (!_healthy) return false;

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    _state = SensorPowerState::Ready;
  }

  return _state == SensorPowerState::Ready;
}

bool Icm20948Sensor::sample() {
  if (!ready()) return false;

  IIcm20948Driver::Data data;
  if (!_driver.read(data) || !data.valid) {
    _reading.valid = false;
    return false;
  }

  static_cast<IIcm20948Driver::Data &>(_reading) = data;
  _reading.timestampMs = _clock.millis();
  _lastSampleMs = _clock.millis();

  return true;
}

bool Icm20948Sensor::ready() const {
  return _healthy && _state == SensorPowerState::Ready &&
         _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Icm20948Sensor::healthy() const { return _healthy; }
SensorPowerState Icm20948Sensor::powerState() const { return _state; }
SensorDutyClass Icm20948Sensor::dutyClass() const { return _cfg.dutyClass; }
const Icm20948Sensor::Reading &Icm20948Sensor::reading() const { return _reading; }
const void *Icm20948Sensor::readingData() const { return &_reading; }
size_t Icm20948Sensor::readingSize() const { return sizeof(Reading); }

size_t Icm20948Sensor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) return 0;

  int n = snprintf(out, maxLen,
                   "imu,ax=%.3f,ay=%.3f,az=%.3f,gx=%.3f,gy=%.3f,gz=%.3f,valid=%u,t_ms=%lu",
                   _reading.accelX,
                   _reading.accelY,
                   _reading.accelZ,
                   _reading.gyroX,
                   _reading.gyroY,
                   _reading.gyroZ,
                   _reading.valid ? 1 : 0,
                   static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) return 0;
  return static_cast<size_t>(n) >= maxLen ? maxLen - 1 : static_cast<size_t>(n);
}

void Icm20948Sensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    snap.imuValid = false;
    return;
  }

  // Driver reports magnetometer in uT and accelerometer in g.
  snap.magX = clampToInt16(_reading.magX * 10.0f);      // uT x 10
  snap.magY = clampToInt16(_reading.magY * 10.0f);
  snap.magZ = clampToInt16(_reading.magZ * 10.0f);
  snap.accelX = clampToInt16(_reading.accelX * 1000.0f); // mg
  snap.accelY = clampToInt16(_reading.accelY * 1000.0f);
  snap.accelZ = clampToInt16(_reading.accelZ * 1000.0f);
  snap.imuValid = true;
  snap.sensorFlags |= 0x08; // IMU
}
