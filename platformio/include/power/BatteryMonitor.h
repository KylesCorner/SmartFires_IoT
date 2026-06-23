// ---
// description: Battery voltage/percent monitor — reads the divider ADC pin, computes battery voltage/percent/low-flag from PowerConfig constants.
// role: implementation
// ---
#pragma once

#include "config/PowerConfig.h"
#include "interfaces/IAnalogReader.h"
#include "interfaces/IClock.h"
#include <Arduino.h>

#include <stddef.h>
#include <stdint.h>

#define PIN_BATTERY_ADC A7

class BatteryMonitor {
public:
  struct Config {
    int pin;

    float adcRefVolts;
    uint16_t adcMax;

    // For voltage divider:
    // battery -> R1 -> ADC pin -> R2 -> GND
    // dividerRatio = (R1 + R2) / R2
    float dividerRatio;

    float minVoltage;
    float maxVoltage;
    float lowVoltage;

    uint32_t minSamplePeriodMs;

    // Thin wrapper: defaults come from config/PowerConfig.h.
    static BatteryMonitor::Config makeBatConfig(
        float adcRefVolts_ = PowerConfig::Battery::kAdcRefVolts,
        uint16_t adcMax_ = PowerConfig::Battery::kAdcMax,
        float dividerRatio_ = PowerConfig::Battery::kDividerRatio,
        float minVoltage_ = PowerConfig::Battery::kMinVoltage,
        float maxVoltage_ = PowerConfig::Battery::kMaxVoltage,
        float lowVoltage_ = PowerConfig::Battery::kLowVoltage,
        uint32_t minSamplePeriodMs_ = PowerConfig::Battery::kMinSamplePeriodMs) {
      BatteryMonitor::Config cfg;
      cfg.pin = PIN_BATTERY_ADC;
      cfg.adcRefVolts = adcRefVolts_;
      cfg.adcMax = adcMax_;
      cfg.dividerRatio = dividerRatio_;
      cfg.minVoltage = minVoltage_;
      cfg.maxVoltage = maxVoltage_;
      cfg.lowVoltage = lowVoltage_;
      cfg.minSamplePeriodMs = minSamplePeriodMs_;
      return cfg;
    }
  };

  struct Reading {
    int raw = 0;
    float adcVolts = 0.0f;
    float batteryVolts = 0.0f;
    float percent = 0.0f;
    bool low = false;
    bool valid = false;
    uint32_t timestampMs = 0;
  };

  BatteryMonitor(const Config &cfg, IAnalogReader &analog, IClock &clock);

  bool begin();
  bool sample();
  bool ready() const;
  bool healthy() const;

  const Reading &reading() const;

  size_t writeTelemetry(char *out, size_t maxLen) const;

private:
  Config _cfg;
  IAnalogReader &_analog;
  IClock &_clock;

  Reading _reading;

  bool _healthy = false;
  uint32_t _lastSampleMs = 0;

  float clampPercent(float percent) const;
};
