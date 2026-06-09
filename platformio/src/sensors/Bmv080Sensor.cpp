#include "sensors/Bmv080Sensor.h"

#include "logging/DebugLogger.h"

#include <Arduino.h>
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

Bmv080Sensor::Bmv080Sensor(const Config &cfg, IBmv080Driver &driver,
                           IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Bmv080Sensor::name() const { return "bmv080"; }

bool Bmv080Sensor::begin() {
  LOG_INFO("bmv080",
           "begin_start addr=0x%02X duty_class=%s min_sample_period_ms=%lu "
           "wake_delay_ms=%lu",
           static_cast<unsigned int>(_cfg.address),
           sensorDutyClassName(_cfg.dutyClass),
           static_cast<unsigned long>(_cfg.minSamplePeriodMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  _healthy = _driver.begin(_cfg.address);

  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_ERROR("bmv080",
              "begin_failed addr=0x%02X reason=driver_begin_failed state=%s",
              static_cast<unsigned int>(_cfg.address),
              sensorPowerStateName(_state));

    return false;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("bmv080", "begin_ok state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  return true;
}

bool Bmv080Sensor::wake() {
  if (!_healthy) {
    LOG_WARN("bmv080", "wake_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Ready ||
      _state == SensorPowerState::Waking) {
    LOG_DEBUG("bmv080", "wake_skip state=%s reason=already_awake",
              sensorPowerStateName(_state));
    return true;
  }

  LOG_DEBUG("bmv080", "wake_start state=%s", sensorPowerStateName(_state));

  if (!_driver.startMeasurement()) {
    _state = SensorPowerState::Error;
    _healthy = false;

    LOG_ERROR("bmv080",
              "wake_failed reason=start_measurement_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;

  LOG_INFO("bmv080", "wake_ok state=%s wake_start_ms=%lu wake_delay_ms=%lu",
           sensorPowerStateName(_state),
           static_cast<unsigned long>(_wakeStartMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  return true;
}

bool Bmv080Sensor::sleep() {
  if (!_healthy) {
    LOG_WARN("bmv080", "sleep_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Sleeping) {
    LOG_DEBUG("bmv080", "sleep_skip state=%s reason=already_sleeping",
              sensorPowerStateName(_state));
    return true;
  }

  LOG_DEBUG("bmv080", "sleep_start state=%s", sensorPowerStateName(_state));

  if (!_driver.stopMeasurement()) {
    _state = SensorPowerState::Error;
    _healthy = false;

    LOG_ERROR("bmv080", "sleep_failed reason=stop_measurement_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("bmv080", "sleep_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool Bmv080Sensor::service() {
  if (!_healthy) {
    LOG_TRACE("bmv080", "service_skip reason=not_healthy state=%u",
              static_cast<unsigned int>(_state));
    return false;
  }

  if (_state == SensorPowerState::Waking) {
    const uint32_t elapsedMs = _clock.millis() - _wakeStartMs;

    if (elapsedMs >= _cfg.wakeDelayMs) {
      _state = SensorPowerState::Ready;

      LOG_INFO("bmv080", "wake_complete elapsed_ms=%lu state=Ready",
               static_cast<unsigned long>(elapsedMs));
    } else {
    //   LOG_DEBUG("bmv080", "wake_wait elapsed_ms=%lu wake_delay_ms=%lu",
    //             static_cast<unsigned long>(elapsedMs),
    //             static_cast<unsigned long>(_cfg.wakeDelayMs));
    }
  }

  return _state == SensorPowerState::Ready;
}
bool Bmv080Sensor::sample() {
  if (!ready()) {
    LOG_TRACE("bmv080",
              "sample_skip reason=not_ready healthy=%u state=%u elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
              _healthy ? 1 : 0,
              static_cast<unsigned int>(_state),
              static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
    return false;
  }

  IBmv080Driver::Data data;

  if (!_driver.read(data)) {
    _reading.valid = false;
    _lastSampleMs = _clock.millis();

    LOG_WARN("bmv080", "sample_failed reason=driver_read_failed");

    return false;
  }

  if (!data.valid) {
    _reading.valid = false;
    _lastSampleMs = _clock.millis();

    LOG_WARN("bmv080", "sample_failed reason=driver_data_invalid");

    return false;
  }

  const uint32_t now = _clock.millis();

  _reading.pm1_0 = data.pm1_0;
  _reading.pm2_5 = data.pm2_5;
  _reading.pm10_0 = data.pm10_0;
  _reading.obstructed = data.obstructed;
  _reading.valid = true;
  _reading.timestampMs = now;

  _lastSampleMs = now;

//   LOG_INFO("bmv080",
//            "sample_ok pm1=%.2f pm25=%.2f pm10=%.2f obstructed=%u t_ms=%lu",
//            _reading.pm1_0,
//            _reading.pm2_5,
//            _reading.pm10_0,
//            _reading.obstructed ? 1 : 0,
//            static_cast<unsigned long>(_reading.timestampMs));

  return true;
}

bool Bmv080Sensor::reset() {
  LOG_INFO("bmv080", "reset_start state=%s", sensorPowerStateName(_state));

  if (!_driver.reset()) {
    _reading.valid = false;
    _healthy = false;
    _state = SensorPowerState::Error;

    LOG_ERROR("bmv080", "reset_failed state=%s", sensorPowerStateName(_state));

    return false;
  }

  _reading = Reading{};
  _healthy = true;
  _state = SensorPowerState::Sleeping;
  _wakeStartMs = 0;
  _lastSampleMs = 0;

  LOG_INFO("bmv080", "reset_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool Bmv080Sensor::ready() const {
  return _healthy && _state == SensorPowerState::Ready &&
         _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Bmv080Sensor::healthy() const { return _healthy; }

SensorPowerState Bmv080Sensor::powerState() const { return _state; }

SensorDutyClass Bmv080Sensor::dutyClass() const { return _cfg.dutyClass; }

const Bmv080Sensor::Reading &Bmv080Sensor::reading() const {
  return _reading;
}

const void *Bmv080Sensor::readingData() const { return &_reading; }

size_t Bmv080Sensor::readingSize() const { return sizeof(Reading); }

size_t Bmv080Sensor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) {
    return 0;
  }

  const int n = snprintf(
      out, maxLen,
      "bmv080,pm1=%.2f,pm25=%.2f,pm10=%.2f,obstructed=%u,valid=%u,t_ms=%lu",
      _reading.pm1_0, _reading.pm2_5, _reading.pm10_0,
      _reading.obstructed ? 1 : 0,
      _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) {
    return 0;
  }

  return static_cast<size_t>(n) >= maxLen ? maxLen - 1
                                          : static_cast<size_t>(n);
}

void Bmv080Sensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    LOG_TRACE("bmv080", "snapshot_skip reason=invalid_reading");
    return;
  }

  snap.pm1_0 = _reading.pm1_0;
  snap.pm2_5 = _reading.pm2_5;
  snap.pm4_0 = NAN;
  snap.pm10 = _reading.pm10_0;

  // Reuse existing particulate-matter bit. Rename the comment later from
  // SPS30 to PM/BMV080 if desired.
  snap.sensorFlags |= 0x10;

  LOG_TRACE("bmv080", "snapshot_fill flags=0x%04X",
            static_cast<unsigned int>(snap.sensorFlags));
}