#pragma once

#include "drivers/IBmv080Driver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "telemetry/SensorSnapshot.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

class Bmv080Sensor final : public ISensor {
public:
  struct Config {
    uint8_t address;
    uint32_t minSamplePeriodMs;
    uint32_t wakeDelayMs;
    SensorDutyClass dutyClass;

    static Bmv080Sensor::Config
    makeBmv080Cfg(uint8_t address_ = 0x54,
                  uint32_t minSamplePeriodMs_ = 1000,
                  uint32_t wakeDelayMs_ = 1000,
                  SensorDutyClass dutyClass_ = SensorDutyClass::AlwaysOn) {
      Bmv080Sensor::Config cfg;
      cfg.address = address_;
      cfg.minSamplePeriodMs = minSamplePeriodMs_;
      cfg.wakeDelayMs = wakeDelayMs_;
      cfg.dutyClass = dutyClass_;
      return cfg;
    }
  };

  struct Reading {
    float pm1_0 = NAN;
    float pm2_5 = NAN;
    float pm10_0 = NAN;
    bool obstructed = false;
    bool valid = false;
    uint32_t timestampMs = 0;
  };

  Bmv080Sensor(const Config &cfg, IBmv080Driver &driver, IClock &clock);

  const char *name() const override;

  bool begin() override;
  bool wake() override;
  bool sleep() override;
  bool service() override;
  bool sample() override;
  bool reset() override;

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
  IBmv080Driver &_driver;
  IClock &_clock;

  Reading _reading;

  bool _healthy = false;
  SensorPowerState _state = SensorPowerState::Off;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
};