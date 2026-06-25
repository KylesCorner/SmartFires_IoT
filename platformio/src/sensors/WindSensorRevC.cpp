// ---
// description: Implements WindSensorRevC's ADC sampling, Rev C wind-speed/temperature regression math, and SensorSnapshot fill.
// role: implementation
// ---
#include "sensors/WindSensorRevC.h"

#include "logging/DebugLogger.h"

#include <stdio.h>

namespace {

constexpr float kMphToMps = 0.44704f;
constexpr uint16_t kWindSensorFlag = 0x01;

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

WindSensorRevC::WindSensorRevC(const Config &cfg,
                               IAnalogReader &analog,
                               Itps &power,
                               IClock &clock)
    : _cfg(cfg), _analog(analog), _power(power), _clock(clock) {}

const char *WindSensorRevC::name() const {
  return "wind";
}

bool WindSensorRevC::begin() {
  LOG_INFO("wind",
           "begin_start duty_class=%s pin_rv=%u pin_tmp=%u adc_ref_v=%.3f "
           "adc_max=%u min_sample_period_ms=%lu wake_delay_ms=%lu",
           sensorDutyClassName(_cfg.dutyClass),
           static_cast<unsigned int>(_cfg.pinRv),
           static_cast<unsigned int>(_cfg.pinTmp),
           _cfg.adcRefVolts,
           static_cast<unsigned int>(_cfg.adcMax),
           static_cast<unsigned long>(_cfg.minSamplePeriodMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  _healthy = _power.begin();

  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_ERROR("wind", "begin_failed reason=power_begin_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _hasSampled = false;
  _lastSampleMs = 0;

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn) {
    if (!_power.enable()) {
      _healthy = false;
      _state = SensorPowerState::Error;

      LOG_ERROR("wind", "begin_failed reason=power_enable_failed state=%s",
                sensorPowerStateName(_state));

      return false;
    }

    _wakeStartMs = _clock.millis();
    _state = SensorPowerState::Waking;

    LOG_INFO("wind",
             "begin_ok always_on_power_enabled state=%s wake_start_ms=%lu "
             "wake_delay_ms=%lu",
             sensorPowerStateName(_state),
             static_cast<unsigned long>(_wakeStartMs),
             static_cast<unsigned long>(_cfg.wakeDelayMs));

    return true;
  }

  if (!_power.disable()) {
    _healthy = false;
    _state = SensorPowerState::Error;

    LOG_ERROR("wind", "begin_failed reason=power_disable_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("wind", "begin_ok state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  return true;
}

bool WindSensorRevC::wake() {
  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_WARN("wind", "wake_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));

    return false;
  }

  if (_state == SensorPowerState::Ready ||
      _state == SensorPowerState::Waking) {
    LOG_DEBUG("wind", "wake_skip state=%s reason=already_awake",
              sensorPowerStateName(_state));
    return true;
  }

  LOG_DEBUG("wind", "wake_start state=%s", sensorPowerStateName(_state));

  if (!_power.enable()) {
    _healthy = false;
    _state = SensorPowerState::Error;

    LOG_ERROR("wind", "wake_failed reason=power_enable_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;

  LOG_INFO("wind", "wake_ok state=%s wake_start_ms=%lu wake_delay_ms=%lu",
           sensorPowerStateName(_state),
           static_cast<unsigned long>(_wakeStartMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  return true;
}

bool WindSensorRevC::sleep() {
  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_WARN("wind", "sleep_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));

    return false;
  }

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn) {
    _state = SensorPowerState::Ready;

    LOG_DEBUG("wind", "sleep_skip reason=always_on state=%s",
              sensorPowerStateName(_state));

    return true;
  }

  if (_state == SensorPowerState::Sleeping) {
    LOG_DEBUG("wind", "sleep_skip state=%s reason=already_sleeping",
              sensorPowerStateName(_state));
    return true;
  }

  LOG_DEBUG("wind", "sleep_start state=%s", sensorPowerStateName(_state));

  if (!_power.disable()) {
    _healthy = false;
    _state = SensorPowerState::Error;

    LOG_ERROR("wind", "sleep_failed reason=power_disable_failed state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  _state = SensorPowerState::Sleeping;

  LOG_INFO("wind", "sleep_ok state=%s", sensorPowerStateName(_state));

  return true;
}

bool WindSensorRevC::service() {
  if (!_healthy) {
    _state = SensorPowerState::Error;

    LOG_TRACE("wind", "service_skip reason=not_healthy state=%s",
              sensorPowerStateName(_state));

    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    const uint32_t elapsedMs = _clock.millis() - _wakeStartMs;

    _state = SensorPowerState::Ready;

    LOG_INFO("wind", "wake_complete elapsed_ms=%lu state=%s",
             static_cast<unsigned long>(elapsedMs),
             sensorPowerStateName(_state));
  }

  return _state == SensorPowerState::Ready;
}

bool WindSensorRevC::sample() {
  if (!ready()) {
    LOG_TRACE("wind",
              "sample_skip reason=not_ready healthy=%u state=%s has_sampled=%u "
              "elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
              _healthy ? 1 : 0,
              sensorPowerStateName(_state),
              _hasSampled ? 1 : 0,
              static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
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

  if (!_reading.valid) {
    LOG_WARN("wind",
             "sample_invalid raw_rv=%d raw_tmp=%d rv_nan=%u tmp_nan=%u "
             "temp_nan=%u zero_nan=%u mph_nan=%u mps_nan=%u",
             _reading.rawRv,
             _reading.rawTmp,
             isnan(_reading.rvVolts) ? 1 : 0,
             isnan(_reading.tmpVolts) ? 1 : 0,
             isnan(_reading.tempC) ? 1 : 0,
             isnan(_reading.zeroWindVolts) ? 1 : 0,
             isnan(_reading.windMph) ? 1 : 0,
             isnan(_reading.windMps) ? 1 : 0);
  }

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
    LOG_TRACE("wind", "snapshot_skip reason=invalid_reading");
    return;
  }

  snap.sensorFlags |= kWindSensorFlag;
  snap.windMps = _reading.windMps;

  // This temp is from the wind sensor compensation thermistor.
  // Only use it as the packet temp if SHT31 did not already populate tempC.
  if (isnan(snap.tempC)) {
    snap.tempC = _reading.tempC;

    LOG_TRACE("wind", "snapshot_fill flags=0x%04X temp_source=wind",
              static_cast<unsigned int>(snap.sensorFlags));
  } else {
    LOG_TRACE("wind", "snapshot_fill flags=0x%04X temp_source=existing",
              static_cast<unsigned int>(snap.sensorFlags));
  }
}

float WindSensorRevC::adcToSensorVolts(int raw, float dividerRatio) const {
  if (raw < 0 ||
      raw > static_cast<int>(_cfg.adcMax) ||
      _cfg.adcMax == 0 ||
      _cfg.adcRefVolts <= 0.0f ||
      dividerRatio <= 0.0f) {
    LOG_WARN("wind",
             "adc_to_sensor_volts_invalid raw=%d divider_ratio=%.3f "
             "adc_ref_v=%.3f adc_max=%u",
             raw,
             dividerRatio,
             _cfg.adcRefVolts,
             static_cast<unsigned int>(_cfg.adcMax));
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
    LOG_WARN("wind",
             "volts_to_formula_adc_units_invalid volts_nan=%u "
             "formula_ref_v=%.3f formula_adc_max=%u",
             isnan(volts) ? 1 : 0,
             _cfg.formulaRefVolts,
             static_cast<unsigned int>(_cfg.formulaAdcMax));
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
    LOG_WARN("wind",
             "estimate_temp_failed reason=tmp_adc_invalid raw_tmp=%d "
             "adc_ref_v=%.3f adc_max=%u formula_ref_v=%.3f formula_adc_max=%u",
             _reading.rawTmp,
             _cfg.adcRefVolts,
             static_cast<unsigned int>(_cfg.adcMax),
             _cfg.formulaRefVolts,
             static_cast<unsigned int>(_cfg.formulaAdcMax));
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
    LOG_WARN("wind",
             "estimate_zero_wind_failed reason=tmp_adc_invalid raw_tmp=%d "
             "adc_ref_v=%.3f adc_max=%u formula_ref_v=%.3f formula_adc_max=%u",
             _reading.rawTmp,
             _cfg.adcRefVolts,
             static_cast<unsigned int>(_cfg.adcMax),
             _cfg.formulaRefVolts,
             static_cast<unsigned int>(_cfg.formulaAdcMax));
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

float WindSensorRevC::estimateWindMph(float rvVolts,
                                      float zeroWindVolts) const {
  if (isnan(rvVolts) || isnan(zeroWindVolts)) {
    LOG_WARN("wind", "estimate_wind_failed rv_nan=%u zero_nan=%u",
             isnan(rvVolts) ? 1 : 0,
             isnan(zeroWindVolts) ? 1 : 0);
    return NAN;
  }

  const float delta = rvVolts - zeroWindVolts;

  if (delta <= 0.0f) {
    return 0.0f;
  }

  return powf(delta / 0.2300f, 2.7265f);
}
