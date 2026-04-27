#pragma once
#include "ISensor.h"
#include <Arduino.h>
#include <math.h>
#include "IWarmup.h"
#include "PinMapping.h"

class WindSensorRevC : public ISensor , public IWarmup{
public:
  struct Config {
    uint8_t pinRv = 0;
    uint8_t pinTmp = 0;

    float adcRefVolts = 3.3f;
    uint16_t adcMax = 4095;

    // float rvDividerRatio = 1.0f;
    // float tmpDividerRatio = 1.0f;
    float rvDividerRatio = (15.0f + 22.0f) / 22.0f;  // ≈ 1.68
    float tmpDividerRatio = 1.68f;

    float zeroWindAdjustmentVolts = 0.2f;

    uint32_t warmupMs = 10000;
    uint32_t minSamplePeriodMs = 200;
    uint8_t samplesToAverage = 8;
    // Optional GPIO used to control external power switch for sensor VCC.
    // Set to 255 to disable power control.
    uint8_t pinPowerEnable = PIN_WIND_POWER;

    // True  -> HIGH turns sensor power ON
    // False -> LOW turns sensor power ON
    bool powerEnableActiveHigh = true;

  };

  // Full config constructor
  explicit WindSensorRevC(const Config &cfg) : cfg_(cfg) {}

  // Simple global-friendly constructor
  WindSensorRevC(uint8_t pinRv, uint8_t pinTmp, float adcRefVolts = 3.3f,
                 uint16_t adcMax = 4095, float rvDividerRatio = 1.0f,
                 float tmpDividerRatio = 1.0f,
                 float zeroWindAdjustmentVolts = 0.2f,
                 uint32_t warmupMs = 10000, uint32_t minSamplePeriodMs = 200,
                 uint8_t samplesToAverage = 8) {
    cfg_.pinRv = pinRv;
    cfg_.pinTmp = pinTmp;
    cfg_.adcRefVolts = adcRefVolts;
    cfg_.adcMax = adcMax;
    cfg_.rvDividerRatio = rvDividerRatio;
    cfg_.tmpDividerRatio = tmpDividerRatio;
    cfg_.zeroWindAdjustmentVolts = zeroWindAdjustmentVolts;
    cfg_.warmupMs = warmupMs;
    cfg_.minSamplePeriodMs = minSamplePeriodMs;
    cfg_.samplesToAverage = samplesToAverage;
  }

  const char *name() const override { return "Wind Sensor Rev C"; }

  bool begin() override {
    pinMode(cfg_.pinRv, INPUT);
    pinMode(cfg_.pinTmp, INPUT);
    startedAtMs_ = millis();
    lastSampleMs_ = 0;
    lastGoodSampleMs_ = 0;
    hasReading_ = false;
    healthy_ = true;
    consecutiveFailures_ = 0;

    if (hasPowerControl()) {
      pinMode(cfg_.pinPowerEnable, OUTPUT);
      powerOn();
    }
    return true;
  }

  bool ready() const override {
    if (_sleeping) {
      return false;
    }
    return (millis() - startedAtMs_) >= cfg_.warmupMs;
  }

  bool sample() override {
    if (_sleeping) {
      return false;
    }
    const uint32_t now = millis();

    if ((now - lastSampleMs_) < cfg_.minSamplePeriodMs) {
      return false;
    }
    lastSampleMs_ = now;

    if (!ready()) {
      return false;
    }

    const uint16_t rvRaw = readAveraged(cfg_.pinRv, cfg_.samplesToAverage);
    const uint16_t tmpRaw = readAveraged(cfg_.pinTmp, cfg_.samplesToAverage);

    if (rvRaw > cfg_.adcMax || tmpRaw > cfg_.adcMax) {
      markFailure();
      return false;
    }

    const float rvAdcVolts = countsToVolts(rvRaw);
    const float tmpAdcVolts = countsToVolts(tmpRaw);

    rvVolts_ = rvAdcVolts * cfg_.rvDividerRatio;
    tmpVolts_ = tmpAdcVolts * cfg_.tmpDividerRatio;

    temperatureC_ = (2.097152f * tmpVolts_ * tmpVolts_) -
                    (34.533376f * tmpVolts_) + 90.754f;

    zeroWindVolts_ = (-0.12288f * tmpVolts_ * tmpVolts_) +
                     (1.0727f * tmpVolts_) + 0.23033203125f -
                     cfg_.zeroWindAdjustmentVolts;

    float normalized = (rvVolts_ - zeroWindVolts_) / 0.2300f;
    if (normalized < 0.0f) {
      normalized = 0.0f;
    }

    windMph_ = powf(normalized, 2.7265f);
    if (!isfinite(windMph_) || windMph_ < 0.0f) {
      windMph_ = 0.0f;
    }

    windMps_ = windMph_ * 0.44704f;

    hasReading_ = true;
    healthy_ = true;
    consecutiveFailures_ = 0;
    lastGoodSampleMs_ = now;
    return true;
  }

  bool hasReading() const override { return hasReading_; }

  uint32_t ageMs() const override {
    if (!hasReading_)
      return UINT32_MAX;
    return millis() - lastGoodSampleMs_;
  }

  bool healthy() const override { return healthy_; }

  float windMph() const { return windMph_; }
  float windMps() const { return windMps_; }
  float temperatureC() const { return temperatureC_; }
  float temperatureF() const { return (temperatureC_ * 9.0f / 5.0f) + 32.0f; }

  float rvVolts() const { return rvVolts_; }
  float tmpVolts() const { return tmpVolts_; }
  float zeroWindVolts() const { return zeroWindVolts_; }

  void setZeroWindAdjustmentVolts(float v) { cfg_.zeroWindAdjustmentVolts = v; }

  // bool sleep() override {
  //   _sleeping = true;
  //   return true;
  // }
  //
  // bool wake() override {
  //   _sleeping = false;
  //   return true;
  // }
  bool sleep() override {
    if (_sleeping) {
      return true;
    }

    _sleeping = true;

    if (hasPowerControl()) {
      powerOff();
    }

    // Invalidate last reading since sensor is no longer powered/warmed.
    hasReading_ = false;
    rvVolts_ = NAN;
    tmpVolts_ = NAN;
    zeroWindVolts_ = NAN;
    temperatureC_ = NAN;
    windMph_ = 0.0f;
    windMps_ = 0.0f;

    return true;
  }

  bool wake() override {
    if (!_sleeping) {
      return true;
    }

    if (hasPowerControl()) {
      powerOn();
    }

    _sleeping = false;

    // Treat as a fresh power-up.
    startedAtMs_ = millis();
    lastSampleMs_ = 0;
    lastGoodSampleMs_ = 0;
    hasReading_ = false;
    healthy_ = true;
    consecutiveFailures_ = 0;

    rvVolts_ = NAN;
    tmpVolts_ = NAN;
    zeroWindVolts_ = NAN;
    temperatureC_ = NAN;
    windMph_ = 0.0f;
    windMps_ = 0.0f;

    return true;
  }
  IWarmup* warmup() override { return this; }
  const IWarmup* warmup() const override { return this; }
  bool requiresPriorityWarmup() const override { return true; }
  bool warmupComplete() const override { return ready(); }

private:
  Config cfg_;

  uint32_t startedAtMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t lastGoodSampleMs_ = 0;

  bool hasReading_ = false;
  bool healthy_ = true;
  uint8_t consecutiveFailures_ = 0;

  float windMph_ = 0.0f;
  float windMps_ = 0.0f;
  float temperatureC_ = NAN;
  float rvVolts_ = NAN;
  float tmpVolts_ = NAN;
  float zeroWindVolts_ = NAN;

  bool _sleeping = false;

  uint16_t readAveraged(uint8_t pin, uint8_t n) {
    if (n == 0)
      n = 1;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < n; ++i) {
      sum += analogRead(pin);
    }
    return static_cast<uint16_t>(sum / n);
  }

  float countsToVolts(uint16_t counts) const {
    return (static_cast<float>(counts) * cfg_.adcRefVolts) /
           static_cast<float>(cfg_.adcMax);
  }

  void markFailure() {
    if (consecutiveFailures_ < 255) {
      ++consecutiveFailures_;
    }
    if (consecutiveFailures_ >= 5) {
      healthy_ = false;
    }
  }
  bool hasPowerControl() const {
    return cfg_.pinPowerEnable != 255;
  }

  void powerOn() {
    if (!hasPowerControl())
      return;
    digitalWrite(cfg_.pinPowerEnable,
                 cfg_.powerEnableActiveHigh ? HIGH : LOW);
  }

  void powerOff() {
    if (!hasPowerControl())
      return;
    digitalWrite(cfg_.pinPowerEnable,
                 cfg_.powerEnableActiveHigh ? LOW : HIGH);
  }
};
