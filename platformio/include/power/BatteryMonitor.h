#pragma once

#include "interfaces/IAnalogReader.h"
#include "interfaces/IClock.h"

#include <stddef.h>
#include <stdint.h>

class BatteryMonitor {
public:
  struct Config {
    uint8_t pin;

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

    static BatteryMonitor::Config
    makeBatConfig(uint8_t pin_ = 0, float adcRefVolts_ = 3.3f,
                  uint16_t adcMax_ = 1023, float dividerRatio_ = 2.0f,
                  float minVoltage_ = 3.2f, float maxVoltage_ = 3.7f,
                  float lowVoltage_ = 3.5f, uint32_t minSamplePeriodMs_=1000) {
      BatteryMonitor::Config cfg;
      cfg.pin = pin_;
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
