// ---
// description: Implements Sht31Sensor's begin/wake/sleep/sample lifecycle, trigger-reading exposure, and SensorSnapshot fill.
// role: implementation
// ---
#include "sensors/Sht31Sensor.h"

#include "interfaces/ISensor.h"
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

Sht31Sensor::Sht31Sensor(const Config &cfg, ISht31Driver &driver, IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Sht31Sensor::name() const { return "sht31"; }

bool Sht31Sensor::begin() {
  LOG_INFO("sht31",
           "begin_start addr=0x%02X duty_class=%s min_sample_period_ms=%lu",
           static_cast<unsigned int>(_cfg.address),
           sensorDutyClassName(_cfg.dutyClass),
           static_cast<unsigned long>(_cfg.minSamplePeriodMs));

  _healthy = _driver.begin(_cfg.address);

  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_ERROR("sht31", "begin_failed addr=0x%02X reason=driver_begin_failed state=%s",
              static_cast<unsigned int>(_cfg.address),
              sensorPowerStateName(_state));

    return false;
  }

  _state = (_cfg.dutyClass == SensorDutyClass::AlwaysOn)
               ? SensorPowerState::Ready
               : SensorPowerState::Sleeping;

  LOG_INFO("sht31", "begin_ok state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  return true;
}

bool Sht31Sensor::wake() {
  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_WARN("sht31", "wake_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));

    return false;
  }

  if (_state == SensorPowerState::Ready) {
    LOG_DEBUG("sht31", "wake_skip state=%s reason=already_ready",
              sensorPowerStateName(_state));
    return true;
  }

  _state = SensorPowerState::Ready;

  LOG_DEBUG("sht31", "wake_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool Sht31Sensor::sleep() {
  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_WARN("sht31", "sleep_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));

    return false;
  }

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn) {
    _state = SensorPowerState::Ready;

    LOG_DEBUG("sht31", "sleep_skip reason=always_on state=%s",
              sensorPowerStateName(_state));

    return true;
  }

  if (_state == SensorPowerState::Sleeping) {
    LOG_DEBUG("sht31", "sleep_skip state=%s reason=already_sleeping",
              sensorPowerStateName(_state));
    return true;
  }

  _state = SensorPowerState::Sleeping;

  LOG_DEBUG("sht31", "sleep_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool Sht31Sensor::service() {
  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_TRACE("sht31", "service_skip reason=not_healthy state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  return _state == SensorPowerState::Ready;
}
bool Sht31Sensor::sample() {
  if (!ready()) {
    LOG_TRACE("sht31",
              "sample_skip reason=not_ready healthy=%u state=%s "
              "elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
              _healthy ? 1 : 0,
              sensorPowerStateName(_state),
              static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
    return false;
  }

  float tempC = NAN;
  float humidityPct = NAN;

  const bool readOk = _driver.read(tempC, humidityPct);

  const bool tempValid =
      isfinite(tempC) && tempC >= -40.0f && tempC <= 125.0f;

  const bool humidityValid =
      isfinite(humidityPct) && humidityPct >= 0.0f && humidityPct <= 100.0f;

  _reading.tempC = tempC;
  _reading.humidityPct = humidityPct;
  _reading.valid = readOk && tempValid && humidityValid;
  _reading.timestampMs = _clock.millis();

  _lastSampleMs = _reading.timestampMs;

  if (!_reading.valid) {
    LOG_WARN("sht31",
             "sample_invalid read_ok=%u temp_c=%.2f humidity_pct=%.2f "
             "temp_finite=%u humidity_finite=%u "
             "temp_in_range=%u humidity_in_range=%u state=%s healthy=%u",
             readOk ? 1 : 0,
             tempC,
             humidityPct,
             isfinite(tempC) ? 1 : 0,
             isfinite(humidityPct) ? 1 : 0,
             tempValid ? 1 : 0,
             humidityValid ? 1 : 0,
             sensorPowerStateName(_state),
             _healthy ? 1 : 0);

    _triggerReading.valid = false;
    return false;
  }

  _triggerReading.valid = true;
  _triggerReading.tempC = _reading.tempC;
  _triggerReading.humidityPct = _reading.humidityPct;

  return true;
}
// bool Sht31Sensor::sample() {
//   if (!ready()) {
//     LOG_TRACE("sht31",
//               "sample_skip reason=not_ready healthy=%u state=%s "
//               "elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
//               _healthy ? 1 : 0,
//               sensorPowerStateName(_state),
//               static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
//               static_cast<unsigned long>(_cfg.minSamplePeriodMs));
//     return false;
//   }

//   const float tempC = _driver.readTemperatureC();
//   const float humidityPct = _driver.readHumidityPct();

//   _reading.tempC = tempC;
//   _reading.humidityPct = humidityPct;
//   const bool tempValid =
//     isfinite(tempC) && tempC >= -40.0f && tempC <= 125.0f;

//   const bool humidityValid =
//       isfinite(humidityPct) && humidityPct >= 0.0f && humidityPct <= 100.0f;

//   _reading.valid = tempValid && humidityValid;
//   _reading.timestampMs = _clock.millis();

//   _triggerReading.valid = _reading.valid;

//   if (_reading.valid) {
//     _triggerReading.tempC = _reading.tempC;
//     _triggerReading.humidityPct = _reading.humidityPct;
//   }

//   _lastSampleMs = _clock.millis();

//   if (!_reading.valid) {
//     LOG_WARN("sht31",
//          "sample_invalid temp_c=%.2f humidity_pct=%.2f "
//          "temp_finite=%u humidity_finite=%u "
//          "temp_in_range=%u humidity_in_range=%u state=%s healthy=%u",
//          tempC,
//          humidityPct,
//          isfinite(tempC) ? 1 : 0,
//          isfinite(humidityPct) ? 1 : 0,
//          tempValid ? 1 : 0,
//          humidityValid ? 1 : 0,
//          sensorPowerStateName(_state),
//          _healthy ? 1 : 0);

//     return false;
//   }

//   return true;
// }

const ITriggerSensor::Reading &Sht31Sensor::triggerReading() const {
  return _triggerReading;
}

bool Sht31Sensor::ready() const {
  if (!_healthy || _state != SensorPowerState::Ready) {
    LOG_TRACE("sht31", "ready_false reason=state_or_health healthy=%u state=%s",
              _healthy ? 1 : 0,
              sensorPowerStateName(_state));
    return false;
  }

  const uint32_t elapsedMs = _clock.millis() - _lastSampleMs;
  const bool canSample = elapsedMs >= _cfg.minSamplePeriodMs;

  if (!canSample) {
    LOG_TRACE("sht31",
              "ready_false reason=min_period elapsed_ms=%lu min_sample_period_ms=%lu",
              static_cast<unsigned long>(elapsedMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
  }

  return canSample;
}

bool Sht31Sensor::healthy() const { return _healthy; }

SensorPowerState Sht31Sensor::powerState() const { return _state; }

SensorDutyClass Sht31Sensor::dutyClass() const { return _cfg.dutyClass; }

const Sht31Sensor::Reading &Sht31Sensor::reading() const { return _reading; }

const void *Sht31Sensor::readingData() const { return &_reading; }

size_t Sht31Sensor::readingSize() const { return sizeof(Reading); }

void Sht31Sensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    LOG_TRACE("sht31", "snapshot_skip reason=invalid_reading");
    return;
  }

  snap.tempC = _reading.tempC;
  snap.humidityPct = _reading.humidityPct;
  snap.sensorFlags |= 0x02; // SHT31

  LOG_TRACE("sht31", "snapshot_fill flags=0x%04X",
            static_cast<unsigned int>(snap.sensorFlags));
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
