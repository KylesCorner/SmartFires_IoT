// ---
// description: Implements BatteryMonitor's ADC sampling, voltage-divider conversion, percent clamping, and telemetry string formatting.
// role: implementation
// ---
#include "power/BatteryMonitor.h"

#include <math.h>
#include <stdio.h>

BatteryMonitor::BatteryMonitor(const Config &cfg, IAnalogReader &analog,
                               IClock &clock)
    : _cfg(cfg), _analog(analog), _clock(clock) {}

bool BatteryMonitor::begin() {
  _healthy = _cfg.adcMax > 0 && _cfg.adcRefVolts > 0.0f &&
             _cfg.dividerRatio > 0.0f && _cfg.maxVoltage > _cfg.minVoltage;

  return _healthy;
}

bool BatteryMonitor::sample() {
  if (!ready()) {
    return false;
  }

  const int raw = _analog.read(_cfg.pin);

  _reading.raw = raw;
  _reading.timestampMs = _clock.millis();

  if (raw < 0 || raw > static_cast<int>(_cfg.adcMax)) {
    _reading.valid = false;
    _healthy = false;
    return false;
  }

  // _reading.adcVolts = (static_cast<float>(raw) * _cfg.adcRefVolts) /
  //                     static_cast<float>(_cfg.adcMax);
  //
  // _reading.batteryVolts = _reading.adcVolts * _cfg.dividerRatio;
  _reading.adcVolts = (static_cast<float>(raw) * _cfg.adcRefVolts) /
                      static_cast<float>(_cfg.adcMax + 1);

  _reading.batteryVolts = _reading.adcVolts * _cfg.dividerRatio;

  const float pct = ((_reading.batteryVolts - _cfg.minVoltage) /
                     (_cfg.maxVoltage - _cfg.minVoltage)) *
                    100.0f;

  _reading.percent = clampPercent(pct);
  _reading.low = _reading.batteryVolts <= _cfg.lowVoltage;
  _reading.valid = !isnan(_reading.batteryVolts);

  _lastSampleMs = _clock.millis();

  return _reading.valid;
}

bool BatteryMonitor::ready() const {
  return _healthy && _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool BatteryMonitor::healthy() const { return _healthy; }

const BatteryMonitor::Reading &BatteryMonitor::reading() const {
  return _reading;
}

size_t BatteryMonitor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) {
    return 0;
  }

  // const int n = snprintf(out, maxLen,
  //                        "battery,v=%.3f,pct=%.1f,low=%u,valid=%u,t_ms=%lu",
  //                        _reading.batteryVolts,
  //                        _reading.percent,
  //                        _reading.low ? 1 : 0,
  //                        _reading.valid ? 1 : 0,
  //                        static_cast<unsigned long>(_reading.timestampMs));
  const int n = snprintf(
      out, maxLen,
      "battery,raw=%d,adc_v=%.3f,v=%.3f,pct=%.1f,low=%u,valid=%u,t_ms=%lu",
      _reading.raw, _reading.adcVolts, _reading.batteryVolts, _reading.percent,
      _reading.low ? 1 : 0, _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) {
    return 0;
  }

  if (static_cast<size_t>(n) >= maxLen) {
    return maxLen - 1;
  }

  return static_cast<size_t>(n);
}

float BatteryMonitor::clampPercent(float percent) const {
  if (percent < 0.0f) {
    return 0.0f;
  }

  if (percent > 100.0f) {
    return 100.0f;
  }

  return percent;
}
