#include "sensors/WindSensorRevC.h"

#include <stdio.h>

namespace {
constexpr float kMphToMps = 0.44704f;
constexpr uint16_t kWindSensorFlag = 0x01;
}

WindSensorRevC::WindSensorRevC(const Config &cfg,
                               IAnalogReader &analog,
                               Itps &power,
                               IClock &clock)
    : _cfg(cfg), _analog(analog), _power(power), _clock(clock) {}

const char *WindSensorRevC::name() const {
  return "wind";
}

bool WindSensorRevC::begin() {
  _healthy = _power.begin();

  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  _hasSampled = false;
  _lastSampleMs = 0;

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn) {
    if (!_power.enable()) {
      _healthy = false;
      _state = SensorPowerState::Error;
      return false;
    }

    _wakeStartMs = _clock.millis();
    _state = SensorPowerState::Waking;
    return true;
  }

  if (!_power.disable()) {
    _healthy = false;
    _state = SensorPowerState::Error;
    return false;
  }

  _state = SensorPowerState::Sleeping;
  return true;
}

bool WindSensorRevC::wake() {
  if (!_healthy) {
    _state = SensorPowerState::Error;
    return false;
  }

  if (_state == SensorPowerState::Ready ||
      _state == SensorPowerState::Waking) {
    return true;
  }

  if (!_power.enable()) {
    _healthy = false;
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

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn) {
    _state = SensorPowerState::Ready;
    return true;
  }

  if (!_power.disable()) {
    _healthy = false;
    _state = SensorPowerState::Error;
    return false;
  }

  _state = SensorPowerState::Sleeping;
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

  const uint32_t now = _clock.millis();

  _reading.rawRv = _analog.read(_cfg.pinRv);
  _reading.rawTmp = _analog.read(_cfg.pinTmp);

  _reading.rvVolts = adcToSensorVolts(_reading.rawRv, _cfg.rvDividerRatio);
  _reading.tmpVolts = adcToSensorVolts(_reading.rawTmp, _cfg.tmpDividerRatio);

  _reading.tempC = estimateTempC(_reading.tmpVolts);
  _reading.zeroWindVolts = estimateZeroWindVolts(_reading.tmpVolts);
  _reading.windMph = estimateWindMph(_reading.rvVolts,
                                     _reading.zeroWindVolts);

  if (!isnan(_reading.windMph)) {
    _reading.windMps = _reading.windMph * kMphToMps;
  } else {
    _reading.windMps = NAN;
  }

  _reading.valid = !isnan(_reading.rvVolts) &&
                   !isnan(_reading.tmpVolts) &&
                   !isnan(_reading.tempC) &&
                   !isnan(_reading.zeroWindVolts) &&
                   !isnan(_reading.windMph) &&
                   !isnan(_reading.windMps);

  _reading.timestampMs = now;
  _lastSampleMs = now;
  _hasSampled = true;

  return _reading.valid;
}

bool WindSensorRevC::ready() const {
  if (!_healthy || _state != SensorPowerState::Ready) {
    return false;
  }

  if (!_hasSampled) {
    return true;
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
      out,
      maxLen,
      "wind,raw_rv=%d,raw_tmp=%d,rv_v=%.3f,tmp_v=%.3f,temp_c=%.2f,"
      "zero_v=%.3f,wind_mph=%.2f,wind_mps=%.2f,valid=%u,t_ms=%lu",
      _reading.rawRv,
      _reading.rawTmp,
      _reading.rvVolts,
      _reading.tmpVolts,
      _reading.tempC,
      _reading.zeroWindVolts,
      _reading.windMph,
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

void WindSensorRevC::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    return;
  }

  snap.sensorFlags |= kWindSensorFlag;
  snap.windMps = _reading.windMps;

  // This temp is from the wind sensor compensation thermistor.
  // Only use it as the packet temp if SHT31 did not already populate tempC.
  if (isnan(snap.tempC)) {
    snap.tempC = _reading.tempC;
  }
}
// float WindSensorRevC::adcToSensorVolts(int raw, float dividerRatio) const {
//   if (raw < 0 ||
//       raw > static_cast<int>(_cfg.adcMax) ||
//       _cfg.adcMax == 0 ||
//       _cfg.adcRefVolts <= 0.0f ||
//       dividerRatio <= 0.0f) {
//     return NAN;
//   }
//
//   const float adcVolts =
//       (static_cast<float>(raw) * _cfg.adcRefVolts) /
//       static_cast<float>(_cfg.adcMax);
//
//   return adcVolts * dividerRatio;
// }

float WindSensorRevC::adcToSensorVolts(int raw, float dividerRatio) const {
  if (raw < 0 ||
      raw > static_cast<int>(_cfg.adcMax) ||
      _cfg.adcMax == 0 ||
      _cfg.adcRefVolts <= 0.0f ||
      dividerRatio <= 0.0f) {
    return NAN;
  }

  const float adcVolts =
      (static_cast<float>(raw) * _cfg.adcRefVolts) /
      static_cast<float>(_cfg.adcMax);

  return adcVolts * dividerRatio;
}

static float rawToAdcPinVolts(int raw, float adcRefVolts, uint16_t adcMax) {
  if (raw < 0 || adcMax == 0 || raw > static_cast<int>(adcMax) ||
      adcRefVolts <= 0.0f) {
    return NAN;
  }

  return (static_cast<float>(raw) * adcRefVolts) /
         static_cast<float>(adcMax);
}
float WindSensorRevC::voltsToFormulaAdcUnits(float volts) const {
  if (isnan(volts) ||
      _cfg.formulaRefVolts <= 0.0f ||
      _cfg.formulaAdcMax == 0) {
    return NAN;
  }

  return (volts / _cfg.formulaRefVolts) *
         static_cast<float>(_cfg.formulaAdcMax);
}
float WindSensorRevC::estimateTempC(float tmpVolts) const {
  (void)tmpVolts;

  const float tmpAd =
      rawToAdcPinVolts(_reading.rawTmp, _cfg.adcRefVolts, _cfg.adcMax) /
      _cfg.formulaRefVolts *
      static_cast<float>(_cfg.formulaAdcMax);

  if (isnan(tmpAd)) {
    return NAN;
  }

  const float tempCtimes100 =
      (0.005f * tmpAd * tmpAd) -
      (16.862f * tmpAd) +
      9075.4f;

  return tempCtimes100 / 100.0f;
}

float WindSensorRevC::estimateZeroWindVolts(float tmpVolts) const {
  (void)tmpVolts;

  const float tmpAd =
      rawToAdcPinVolts(_reading.rawTmp, _cfg.adcRefVolts, _cfg.adcMax) /
      _cfg.formulaRefVolts *
      static_cast<float>(_cfg.formulaAdcMax);

  if (isnan(tmpAd)) {
    return NAN;
  }

  const float zeroWindAd =
      (-0.0006f * tmpAd * tmpAd) +
      (1.0727f * tmpAd) +
      47.172f;

  const float zeroWindVolts =
      (zeroWindAd * _cfg.formulaRefVolts /
       static_cast<float>(_cfg.formulaAdcMax)) -
      _cfg.zeroWindAdjustmentVolts;

  return zeroWindVolts;
}

// float WindSensorRevC::estimateTempC(float tmpVolts) const {
//   const float tmpAd = voltsToFormulaAdcUnits(tmpVolts);
//
//   if (isnan(tmpAd)) {
//     return NAN;
//   }
//
//   const float tempCtimes100 =
//       (0.005f * tmpAd * tmpAd) -
//       (16.862f * tmpAd) +
//       9075.4f;
//
//   return tempCtimes100 / 100.0f;
// }
//
// float WindSensorRevC::estimateZeroWindVolts(float tmpVolts) const {
//   const float tmpAd = voltsToFormulaAdcUnits(tmpVolts);
//
//   if (isnan(tmpAd)) {
//     return NAN;
//   }
//
//   const float zeroWindAd =
//       (-0.0006f * tmpAd * tmpAd) +
//       (1.0727f * tmpAd) +
//       47.172f;
//
//   const float zeroWindVolts =
//       (zeroWindAd * _cfg.formulaRefVolts /
//        static_cast<float>(_cfg.formulaAdcMax)) -
//       _cfg.zeroWindAdjustmentVolts;
//
//   return zeroWindVolts;
// }

float WindSensorRevC::estimateWindMph(float rvVolts,
                                      float zeroWindVolts) const {
  if (isnan(rvVolts) || isnan(zeroWindVolts)) {
    return NAN;
  }

  const float delta = rvVolts - zeroWindVolts;

  if (delta <= 0.0f) {
    return 0.0f;
  }

  return powf(delta / 0.2300f, 2.7265f);
}
