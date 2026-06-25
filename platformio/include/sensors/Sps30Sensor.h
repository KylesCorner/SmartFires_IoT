// ---
// description: ISensor implementation wrapping the SPS30 particulate-matter driver, exposing PM1.0/2.5/4.0/10 readings.
// role: implementation
// ---
#pragma once

#include "drivers/ISps30Driver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "telemetry/SensorSnapshot.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

class Sps30Sensor final : public ISensor {
public:
  struct Config {
    uint32_t minSamplePeriodMs;
    uint32_t wakeDelayMs;
    SensorDutyClass dutyClass;

    // Thin wrapper: defaults come from config/SensingConfig.h's Sps30
    // namespace (this sensor's own independently-tuned values).
    static Sps30Sensor::Config make(
        uint32_t minSamplePeriodMs_ = 1000,
        uint32_t wakeDelayMs_ = 15000,
        SensorDutyClass dutyClass_ = SensorDutyClass::WarmupHeavy) {
      Sps30Sensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs_;
      cfg.wakeDelayMs = wakeDelayMs_;
      cfg.dutyClass = dutyClass_;
      return cfg;
    }
  };

  struct Reading {
    float pm1_0 = NAN;
    float pm2_5 = NAN;
    float pm4_0 = NAN;
    float pm10_0 = NAN;
    bool valid = false;
    uint32_t timestampMs = 0;
  };

  Sps30Sensor(const Config &cfg, ISps30Driver &driver, IClock &clock);

  const char *name() const override;

  bool begin() override;
  bool wake() override;
  bool sleep() override;
  bool service() override;
  bool sample() override;

  bool ready() const override;
  bool healthy() const override;

  SensorPowerState powerState() const override;
  SensorDutyClass dutyClass() const override;

  const Reading &reading() const;

  const void *readingData() const override;
  size_t readingSize() const override;

  size_t writeTelemetry(char *out, size_t maxLen) const override;
  void fillSnapshot(SensorSnapshot &snap) const override;

private:
  Config _cfg;
  ISps30Driver &_driver;
  IClock &_clock;

  Reading _reading;

  bool _healthy = false;
  SensorPowerState _state = SensorPowerState::Off;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
};
