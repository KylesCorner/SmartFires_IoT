// ---
// description: Implements Sps30Sensor's measurement start/stop lifecycle, PM reading sampling, and SensorSnapshot fill.
// role: implementation
// ---
#include "sensors/Sps30Sensor.h"

#include "logging/DebugLogger.h"

#include <stdio.h>

namespace {

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

Sps30Sensor::Sps30Sensor(const Config &cfg, ISps30Driver &driver,
                         IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Sps30Sensor::name() const { return "sps30"; }

bool Sps30Sensor::begin() {
  LOG_INFO("sps30",
           "begin_start duty_class=%s min_sample_period_ms=%lu wake_delay_ms=%lu",
           sensorDutyClassName(_cfg.dutyClass),
           static_cast<unsigned long>(_cfg.minSamplePeriodMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  _healthy = _driver.begin();

  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_ERROR("sps30", "begin_failed reason=driver_begin_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("sps30", "begin_ok state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  return true;
}

bool Sps30Sensor::wake() {
  if (!_healthy) {
    LOG_WARN("sps30", "wake_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Ready ||
      _state == SensorPowerState::Waking) {
    LOG_DEBUG("sps30", "wake_skip state=%s reason=already_awake",
              sensorPowerStateName(_state));
    return true;
  }

  LOG_DEBUG("sps30", "wake_start state=%s", sensorPowerStateName(_state));

  if (!_driver.startMeasurement()) {
    _state = SensorPowerState::Error;
    _healthy = false;

    LOG_ERROR("sps30", "wake_failed reason=start_measurement_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;

  LOG_INFO("sps30", "wake_ok state=%s wake_start_ms=%lu wake_delay_ms=%lu",
           sensorPowerStateName(_state),
           static_cast<unsigned long>(_wakeStartMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  return true;
}

bool Sps30Sensor::sleep() {
  if (!_healthy) {
    LOG_WARN("sps30", "sleep_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Sleeping) {
    LOG_DEBUG("sps30", "sleep_skip state=%s reason=already_sleeping",
              sensorPowerStateName(_state));
    return true;
  }

  LOG_DEBUG("sps30", "sleep_start state=%s", sensorPowerStateName(_state));

  if (!_driver.stopMeasurement()) {
    _state = SensorPowerState::Error;
    _healthy = false;

    LOG_ERROR("sps30", "sleep_failed reason=stop_measurement_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("sps30", "sleep_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool Sps30Sensor::service() {
  if (!_healthy) {
    LOG_TRACE("sps30", "service_skip reason=not_healthy state=%s",
              sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    const uint32_t elapsedMs = _clock.millis() - _wakeStartMs;

    _state = SensorPowerState::Ready;

    LOG_INFO("sps30", "wake_complete elapsed_ms=%lu state=%s",
             static_cast<unsigned long>(elapsedMs),
             sensorPowerStateName(_state));
  }

  return _state == SensorPowerState::Ready;
}

bool Sps30Sensor::sample() {
  if (!ready()) {
    LOG_TRACE("sps30",
              "sample_skip reason=not_ready healthy=%u state=%s "
              "elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
              _healthy ? 1 : 0,
              sensorPowerStateName(_state),
              static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
    return false;
  }

  ISps30Driver::Data data;

  if (!_driver.read(data)) {
    _reading.valid = false;

    LOG_WARN("sps30", "sample_failed reason=driver_read_failed");

    return false;
  }

  if (!data.valid) {
    _reading.valid = false;

    LOG_WARN("sps30", "sample_failed reason=driver_data_invalid");

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
    LOG_TRACE("sps30", "snapshot_skip reason=invalid_reading");
    return;
  }

  snap.pm1_0 = _reading.pm1_0;
  snap.pm2_5 = _reading.pm2_5;
  snap.pm4_0 = _reading.pm4_0;
  snap.pm10 = _reading.pm10_0;

  snap.sensorFlags |= 0x10;

  LOG_TRACE("sps30", "snapshot_fill flags=0x%04X",
            static_cast<unsigned int>(snap.sensorFlags));
}
