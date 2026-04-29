#include "sensors/WindSensorRevC.h"

#include <stdio.h>

WindSensorRevC::WindSensorRevC(const Config &cfg, IAnalogReader &analog,
                               IClock &clock)
    : _cfg(cfg), _analog(analog), _clock(clock) {}

const char *WindSensorRevC::name() const {
  return "wind";
}

bool WindSensorRevC::begin() {
  _healthy = true;

  _state = (_cfg.dutyClass == SensorDutyClass::AlwaysOn)
               ? SensorPowerState::Ready
               : SensorPowerState::Sleeping;

  return true;
}

bool WindSensorRevC::wake() {
  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;
  return true;
}

bool WindSensorRevC::sleep() {
  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  _state = (_cfg.dutyClass == SensorDutyClass::AlwaysOn)
               ? SensorPowerState::Ready
               : SensorPowerState::Sleeping;

  return true;
}

bool WindSensorRevC::service() {
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

bool WindSensorRevC::sample() {
  if (!ready()) {
    return false;
  }

  _reading.rawRv = _analog.read(_cfg.pinRv);
  _reading.rawTmp = _analog.read(_cfg.pinTmp);

  _reading.rvVolts = adcToVolts(_reading.rawRv, _cfg.rvDividerRatio);
  _reading.tmpVolts = adcToVolts(_reading.rawTmp, _cfg.tmpDividerRatio);

  _reading.tempC = estimateTempC(_reading.tmpVolts);
  _reading.windMps = estimateWindMps(_reading.rvVolts, _reading.tempC);

  _reading.valid = !isnan(_reading.rvVolts) && !isnan(_reading.tmpVolts) &&
                   !isnan(_reading.tempC) && !isnan(_reading.windMps);

  _reading.timestampMs = _clock.millis();
  _lastSampleMs = _clock.millis();

  return _reading.valid;
}

bool WindSensorRevC::ready() const {
  if (!_healthy || _state != SensorPowerState::Ready) {
    return false;
  }

  return _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool WindSensorRevC::healthy() const {
  return _healthy;
}

SensorPowerState WindSensorRevC::powerState() const {
  return _state;
}

SensorDutyClass WindSensorRevC::dutyClass() const {
  return _cfg.dutyClass;
}

const WindSensorRevC::Reading &WindSensorRevC::reading() const {
  return _reading;
}

const void *WindSensorRevC::readingData() const {
  return &_reading;
}

size_t WindSensorRevC::readingSize() const {
  return sizeof(Reading);
}

size_t WindSensorRevC::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) {
    return 0;
  }

  const int n = snprintf(
      out, maxLen,
      "wind,rv_v=%.3f,tmp_v=%.3f,temp_c=%.2f,wind_mps=%.2f,valid=%u,t_ms=%lu",
      _reading.rvVolts,
      _reading.tmpVolts,
      _reading.tempC,
      _reading.windMps,
      _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) {
    return 0;
  }

  if (static_cast<size_t>(n) >= maxLen) {
    return maxLen - 1;
  }

  return static_cast<size_t>(n);
}

float WindSensorRevC::adcToVolts(int raw, float dividerRatio) const {
  if (raw < 0 || raw > static_cast<int>(_cfg.adcMax) || dividerRatio <= 0.0f) {
    return NAN;
  }

  const float adcVolts =
      (static_cast<float>(raw) * _cfg.adcRefVolts) /
      static_cast<float>(_cfg.adcMax);

  return adcVolts * dividerRatio;
}

float WindSensorRevC::estimateTempC(float tmpVolts) const {
  if (isnan(tmpVolts)) {
    return NAN;
  }

  // Placeholder linear estimate.
  // Replace with the exact Wind Sensor Rev C thermistor formula later.
  return (tmpVolts - 0.5f) * 100.0f;
}

float WindSensorRevC::estimateWindMps(float rvVolts, float tempC) const {
  if (isnan(rvVolts) || isnan(tempC)) {
    return NAN;
  }

  const float adjusted = rvVolts - _cfg.zeroWindAdjustmentVolts;

  if (adjusted <= 0.0f) {
    return 0.0f;
  }

  // Placeholder approximation.
  // Keep this isolated so the formula can be swapped later.
  return adjusted * 10.0f;
}
