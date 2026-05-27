#include "sensors/Icm20948Sensor.h"

#include "logging/DebugLogger.h"

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

const char *sensorPowerStateName(SensorPowerState state) {
  switch (state) {
  case SensorPowerState::Ready:
    return "Ready";
  case SensorPowerState::Waking:
    return "Waking";
  case SensorPowerState::Sleeping:
    return "Sleeping";
  case SensorPowerState::Error:
    return "Error";
  default:
    return "Unknown";
  }
}

const char *sensorDutyClassName(SensorDutyClass dutyClass) {
  switch (dutyClass) {
  case SensorDutyClass::AlwaysOn:
    return "AlwaysOn";
  case SensorDutyClass::DutyCycled:
    return "DutyCycled";
  case SensorDutyClass::WarmupHeavy:
    return "WarmupHeavy";
  default:
    return "Unknown";
  }
}

} // namespace

Icm20948Sensor::Icm20948Sensor(const Config &cfg, IIcm20948Driver &driver,
                               IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Icm20948Sensor::name() const { return "imu"; }

bool Icm20948Sensor::begin() {
  LOG_INFO("imu",
           "begin_start addr=0x%02X duty_class=%s min_sample_period_ms=%lu "
           "wake_delay_ms=%lu",
           static_cast<unsigned int>(_cfg.address),
           sensorDutyClassName(_cfg.dutyClass),
           static_cast<unsigned long>(_cfg.minSamplePeriodMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  _healthy = _driver.begin(_cfg.address);
  _state = _healthy ? SensorPowerState::Sleeping : SensorPowerState::Error;

  if (!_healthy) {
    LOG_ERROR("imu", "begin_failed addr=0x%02X state=%s",
              static_cast<unsigned int>(_cfg.address),
              sensorPowerStateName(_state));
    return false;
  }

  LOG_INFO("imu", "begin_ok state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  return true;
}

bool Icm20948Sensor::wake() {
  if (!_healthy) {
    LOG_WARN("imu", "wake_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Ready ||
      _state == SensorPowerState::Waking) {
    LOG_DEBUG("imu", "wake_skip state=%s reason=already_awake",
              sensorPowerStateName(_state));
    return true;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;

  LOG_INFO("imu", "wake_ok state=%s wake_start_ms=%lu wake_delay_ms=%lu",
           sensorPowerStateName(_state),
           static_cast<unsigned long>(_wakeStartMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  return true;
}

bool Icm20948Sensor::sleep() {
  if (!_healthy) {
    LOG_WARN("imu", "sleep_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Sleeping) {
    LOG_DEBUG("imu", "sleep_skip state=%s reason=already_sleeping",
              sensorPowerStateName(_state));
    return true;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("imu", "sleep_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool Icm20948Sensor::service() {
  if (!_healthy) {
    LOG_TRACE("imu", "service_skip reason=not_healthy state=%s",
              sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    const uint32_t elapsedMs = _clock.millis() - _wakeStartMs;

    _state = SensorPowerState::Ready;

    LOG_INFO("imu", "wake_complete elapsed_ms=%lu state=%s",
             static_cast<unsigned long>(elapsedMs),
             sensorPowerStateName(_state));
  }

  return _state == SensorPowerState::Ready;
}

bool Icm20948Sensor::sample() {
  if (!ready()) {
    LOG_TRACE("imu",
              "sample_skip reason=not_ready healthy=%u state=%s "
              "elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
              _healthy ? 1 : 0,
              sensorPowerStateName(_state),
              static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
    return false;
  }

  IIcm20948Driver::Data data;

  if (!_driver.read(data)) {
    _reading.valid = false;

    LOG_WARN("imu", "sample_failed reason=driver_read_failed");

    return false;
  }

  if (!data.valid) {
    _reading.valid = false;

    LOG_WARN("imu", "sample_failed reason=driver_data_invalid");

    return false;
  }

  static_cast<IIcm20948Driver::Data &>(_reading) = data;

  _reading.timestampMs = _clock.millis();
  _lastSampleMs = _reading.timestampMs;

  // Dedicated IMU debug stream: emitted per IMU sample so troubleshooting
  // does not depend on STATUS packet interval.
  LOG_DEBUG(
      "IMU",
      "imu_raw_sample mag_ut=[%.3f,%.3f,%.3f] accel_mg=[%.1f,%.1f,%.1f] gyro_dps=[%.3f,%.3f,%.3f] t_ms=%lu",
      _reading.magX,
      _reading.magY,
      _reading.magZ,
      _reading.accelX,
      _reading.accelY,
      _reading.accelZ,
      _reading.gyroX,
      _reading.gyroY,
      _reading.gyroZ,
      static_cast<unsigned long>(_reading.timestampMs));

  return true;
}

bool Icm20948Sensor::ready() const {
  return _healthy && _state == SensorPowerState::Ready &&
         _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Icm20948Sensor::healthy() const { return _healthy; }

SensorPowerState Icm20948Sensor::powerState() const { return _state; }

SensorDutyClass Icm20948Sensor::dutyClass() const { return _cfg.dutyClass; }

const Icm20948Sensor::Reading &Icm20948Sensor::reading() const {
  return _reading;
}

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

  // Driver reports magnetometer in uT and accelerometer in milli-g.
  snap.magX = clampToInt16(_reading.magX * 10.0f);      // uT x 10
  snap.magY = clampToInt16(_reading.magY * 10.0f);
  snap.magZ = clampToInt16(_reading.magZ * 10.0f);
  snap.accelX = clampToInt16(_reading.accelX); // mg
  snap.accelY = clampToInt16(_reading.accelY);
  snap.accelZ = clampToInt16(_reading.accelZ);
  snap.imuValid = true;
  snap.sensorFlags |= 0x08; // IMU
}
